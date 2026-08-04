/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_roundtrip.cpp
 * @brief Fase 0.5: validar el MODELO ABSTRACTO en aislamiento (round-trip).  SIN
 *        codigo real: SSA sintetico -> LiveRanges -> Interference -> Coloring ->
 *        re-abstraer.  Property-based sobre CFGs sinteticos (PRNG determinista).
 *
 * Propiedades verificadas (round-trip IR-level):
 *   1. La ABSTRACCION es una funcion pura determinista (build_interference
 *      idempotente): re-abstraer da SIEMPRE el mismo grafo -> los HECHOS son
 *      reversibles (aunque el coloreado no sea unico).
 *   2. El coloreado producido es SIEMPRE un coloreado PROPIO (is_proper_coloring
 *      via validate_assignment), derrame o no.
 *   3. CROMATICO (interval graph): si el banco ofrece >= max_overlap(clase) lanes
 *      asignables, el coloreador NO derrama ningun valor de esa clase.
 *
 * La reversibilidad a nivel de CODIGO (materializar -> re-lift via ASA -> Facts
 * iguales) queda DIFERIDA hasta que exista el lifter ASA; aqui se cierra el
 * round-trip a nivel IR/modelo, autocontenido.
 */

#include "codegen/rbank/abstract_problem.h"
#include "codegen/rbank/coloring.h"
#include "codegen/rbank/physical_bank.h"

#include <cstdio>
#include <random>
#include <vector>

using namespace jit;            // BackendCaps.
using namespace codegen::rbank; // el modelo.

static int g_checks = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_fail;                                                        \
            std::printf("  [FAIL] %s (linea %d)\n", (msg), __LINE__);        \
        }                                                                    \
    } while (0)

static AbstractValue mkval(uint32_t id, uint32_t start, uint32_t end,
                           ResourceClass cls, ViewWidth w) {
    AbstractValue v;
    v.value_id = id;
    v.start = start;
    v.end = end;
    v.req.value_id = id;
    v.req.cls = cls;
    v.req.width = w;
    return v;
}

int main() {
    std::printf("=== test_rbank_roundtrip (Fase 0.5: modelo abstracto fiel) ===\n");

    const BackendCaps caps = [] { BackendCaps c{}; c.sse2 = true; return c; }();
    PhysicalRegisterBank bank = physical_bank_x86_64(true, caps);

    // --- Unit: abstraccion LiveRanges -> Interference ---
    std::printf("\n[abstraccion: rangos que se solapan interfieren]\n");
    {
        AbstractProblem p;
        // 3 valores GP TODOS vivos a la vez [0,10] -> clique de 3.
        p.values = {mkval(1, 0, 10, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 0, 10, ResourceClass::GP, ViewWidth::W8),
                    mkval(3, 0, 10, ResourceClass::GP, ViewWidth::W8)};
        ConstraintSet inter = build_interference(p);
        CHECK(inter.interferes(1, 2) && inter.interferes(1, 3) && inter.interferes(2, 3),
              "clique de 3 no detectado");
        CHECK(max_overlap(p, ResourceClass::GP) == 3, "max_overlap del clique != 3");
        // Idempotencia: re-abstraer coincide.
        CHECK(same_interference(inter, build_interference(p)),
              "la abstraccion no es determinista (round-trip roto)");
    }
    {
        // Rangos DISJUNTOS (secuenciales) no interfieren -> pueden compartir lane.
        AbstractProblem p;
        p.values = {mkval(1, 0, 5, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 6, 10, ResourceClass::GP, ViewWidth::W8)};
        CHECK(!build_interference(p).interferes(1, 2), "rangos disjuntos interfieren");
        CHECK(max_overlap(p, ResourceClass::GP) == 1, "max_overlap disjuntos != 1");
    }

    // --- Unit: coloreado propio + reuso de lane ---
    std::printf("\n[coloreado: clique separado, disjuntos comparten]\n");
    {
        AbstractProblem p;
        p.values = {mkval(1, 0, 10, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 0, 10, ResourceClass::GP, ViewWidth::W8),
                    mkval(3, 0, 10, ResourceClass::GP, ViewWidth::W8)};
        LaneAssignment col = color_linear_scan(p, bank, false);
        CHECK(is_proper_coloring(p, col, bank, false), "clique mal coloreado");
        CHECK(spill_count(p, col) == 0, "clique de 3 con banco amplio derramo");
        // 3 lanes DISTINTAS.
        CHECK(col.lane_of(1) != col.lane_of(2) && col.lane_of(1) != col.lane_of(3) &&
                  col.lane_of(2) != col.lane_of(3),
              "el clique no uso 3 lanes distintas");
    }
    {
        // Dos disjuntos: el coloreador REUSA la lane liberada.
        AbstractProblem p;
        p.values = {mkval(1, 0, 5, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 6, 10, ResourceClass::GP, ViewWidth::W8)};
        LaneAssignment col = color_linear_scan(p, bank, false);
        CHECK(is_proper_coloring(p, col, bank, false), "disjuntos mal coloreado");
        CHECK(col.lane_of(1) == col.lane_of(2), "disjuntos no reusaron la lane");
    }

    // --- Unit: presion extrema -> derrama pero SIEMPRE valido ---
    std::printf("\n[presion > lanes: derrama pero el coloreado es valido]\n");
    {
        const size_t gp_lanes = bank.allocatable_count(ResourceClass::GP, false);
        AbstractProblem p;
        // gp_lanes+3 valores GP TODOS vivos a la vez -> imposible sin spill.
        for (uint32_t i = 0; i < gp_lanes + 3; ++i)
            p.values.push_back(mkval(i + 1, 0, 100, ResourceClass::GP, ViewWidth::W8));
        LaneAssignment col = color_linear_scan(p, bank, false);
        CHECK(is_proper_coloring(p, col, bank, false),
              "coloreado invalido bajo presion (BUG del coloreador)");
        CHECK(spill_count(p, col) == 3, "no derramo exactamente el exceso (3)");
    }

    // --- PROPERTY-BASED: CFGs sinteticos aleatorios (PRNG determinista) ---
    std::printf("\n[property-based: 1000 problemas sinteticos]\n");
    {
        std::mt19937 rng(0xC0FFEEu); // seed FIJO -> reproducible (no Math.random).
        const size_t gp_alloc = bank.allocatable_count(ResourceClass::GP, false);
        const size_t fp_alloc = bank.allocatable_count(ResourceClass::FP_VECTOR, false);

        int idempotent_ok = 0, proper_ok = 0, chromatic_ok = 0, chromatic_cases = 0;
        const int TRIALS = 1000;
        for (int t = 0; t < TRIALS; ++t) {
            std::uniform_int_distribution<uint32_t> nvals(1, 24);
            std::uniform_int_distribution<uint32_t> pos(0, 40);
            std::uniform_int_distribution<uint32_t> dur(0, 12);
            std::uniform_int_distribution<int> coin(0, 1);

            AbstractProblem p;
            const uint32_t n = nvals(rng);
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t s = pos(rng);
                const uint32_t e = s + dur(rng);
                const bool fp = coin(rng) != 0;
                p.values.push_back(mkval(
                    i + 1, s, e,
                    fp ? ResourceClass::FP_VECTOR : ResourceClass::GP,
                    fp ? ViewWidth::W16 : ViewWidth::W8));
            }

            // 1. Idempotencia de la abstraccion (reversibilidad de los HECHOS).
            if (same_interference(build_interference(p), build_interference(p)))
                ++idempotent_ok;

            // 2. Coloreado SIEMPRE propio.
            LaneAssignment col = color_linear_scan(p, bank, false);
            if (is_proper_coloring(p, col, bank, false)) ++proper_ok;

            // 3. Cromatico: presion <= lanes de la clase -> 0 spills de esa clase.
            for (ResourceClass cls : {ResourceClass::GP, ResourceClass::FP_VECTOR}) {
                const size_t alloc = (cls == ResourceClass::GP) ? gp_alloc : fp_alloc;
                if (max_overlap(p, cls) <= alloc) {
                    ++chromatic_cases;
                    if (spill_count(p, col, cls) == 0) ++chromatic_ok;
                }
            }
        }
        CHECK(idempotent_ok == TRIALS, "la abstraccion no fue determinista en algun caso");
        CHECK(proper_ok == TRIALS, "algun coloreado fue invalido (BUG)");
        CHECK(chromatic_ok == chromatic_cases,
              "presion<=lanes pero derramo (coloreador NO optimo en interval graph)");
        std::printf("  idempotente=%d/%d  propio=%d/%d  cromatico=%d/%d\n",
                    idempotent_ok, TRIALS, proper_ok, TRIALS, chromatic_ok, chromatic_cases);
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
