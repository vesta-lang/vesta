/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_coalesce.cpp
 * @brief Fase 3: coalescing conservador sobre el problema abstracto.  Funde los
 *        valores AFINES (AffinityGraphFacts) que pueden compartir lane sin
 * subir la presion.  Property-based: nunca REINTRODUCE spill + coloreo propio.
 */

#include "codegen/rbank/coalesce.h"
#include "codegen/rbank/coloring.h"
#include "codegen/rbank/physical_bank.h"

#include <cstdio>
#include <random>
#include <vector>

using namespace jit;            // BackendCaps.
using namespace codegen::rbank; // el modelo.

static int g_checks = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fail;                                                          \
            std::printf("  [FAIL] %s (linea %d)\n", (msg), __LINE__);          \
        }                                                                      \
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
    std::printf(
        "=== test_rbank_coalesce (Fase 3: coalescing conservador) ===\n");

    const BackendCaps caps = [] {
        BackendCaps c{};
        c.sse2 = true;
        return c;
    }();
    PhysicalRegisterBank bank = physical_bank_x86_64(true, caps);

    // --- Unit: afines contiguos no-interferentes -> se funden ---
    std::printf("\n[afines contiguos disjuntos -> fundidos, comparten lane]\n");
    {
        AbstractProblem p;
        p.values = {mkval(1, 0, 5, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 6, 10, ResourceClass::GP, ViewWidth::W8)};
        p.affinity.edges = {{1, 2, 1.0f}};
        CoalesceResult c = coalesce_conservative(p);
        CHECK(c.copies_eliminated == 1, "no fundio afines disjuntos");
        CHECK(c.problem.values.size() == 1, "no colapso en 1 grupo");
        CHECK(c.rep[1] == c.rep[2],
              "afines fundidos no comparten representante");
    }

    // --- Unit: afines que INTERFIEREN -> NO se funden ---
    std::printf("\n[afines interferentes -> NO fundidos]\n");
    {
        AbstractProblem p;
        p.values = {mkval(1, 0, 10, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 5, 15, ResourceClass::GP, ViewWidth::W8)};
        p.affinity.edges = {{1, 2, 1.0f}};
        CoalesceResult c = coalesce_conservative(p);
        CHECK(c.copies_eliminated == 0, "fundio dos valores vivos a la vez");
    }

    // --- Unit: el caso del HUECO (envolvente crearia interferencia) ->
    // rechazado ---
    std::printf("\n[afines A,B con C entre medias -> punto (3) lo RECHAZA]\n");
    {
        AbstractProblem p;
        // A=[0,3], B=[10,12] afines (no interfieren); C=[5,8] vive en el hueco.
        p.values = {mkval(1, 0, 3, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 10, 12, ResourceClass::GP, ViewWidth::W8),
                    mkval(3, 5, 8, ResourceClass::GP, ViewWidth::W8)};
        p.affinity.edges = {{1, 2, 1.0f}};
        CoalesceResult c = coalesce_conservative(p);
        CHECK(
            c.copies_eliminated == 0,
            "fundio A-B: el envolvente [0,12] interfiere con C (bug del hull)");
    }

    // --- Unit: clase/ancho distintos -> NO se funden ---
    std::printf("\n[clase distinta -> NO fundidos]\n");
    {
        AbstractProblem p;
        p.values = {mkval(1, 0, 5, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 6, 10, ResourceClass::FP_VECTOR, ViewWidth::W16)};
        p.affinity.edges = {{1, 2, 1.0f}};
        CoalesceResult c = coalesce_conservative(p);
        CHECK(c.copies_eliminated == 0, "fundio valores de clase distinta");
    }

    // --- PROPERTY-BASED: nunca reintroduce spill + coloreo propio ---
    std::printf("\n[property-based: 1000 problemas + afinidades aleatorias]\n");
    {
        std::mt19937 rng(0xC0A1E5Cu);
        int proper_ok = 0, no_new_spill = 0, no_new_spill_cases = 0,
            did_fuse = 0;
        const int TRIALS = 1000;
        for (int t = 0; t < TRIALS; ++t) {
            std::uniform_int_distribution<uint32_t> nvals(2, 16);
            std::uniform_int_distribution<uint32_t> pos(0, 30);
            std::uniform_int_distribution<uint32_t> dur(0, 8);
            std::uniform_int_distribution<int> coin(0, 1);

            AbstractProblem p;
            const uint32_t n = nvals(rng);
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t s = pos(rng);
                const uint32_t e = s + dur(rng);
                const bool fp = coin(rng) != 0;
                p.values.push_back(
                    mkval(i + 1, s, e,
                          fp ? ResourceClass::FP_VECTOR : ResourceClass::GP,
                          fp ? ViewWidth::W16 : ViewWidth::W8));
            }
            // Afinidades aleatorias entre pares de valores.
            std::uniform_int_distribution<uint32_t> pick(1, n);
            const uint32_t nedges =
                std::uniform_int_distribution<uint32_t>(0, n)(rng);
            for (uint32_t k = 0; k < nedges; ++k) {
                uint32_t a = pick(rng), b = pick(rng);
                if (a != b) p.affinity.edges.push_back({a, b, 1.0f});
            }

            CoalesceResult c = coalesce_conservative(p);
            if (c.copies_eliminated > 0) ++did_fuse;

            // El coloreado del problema coalescido es SIEMPRE propio.
            LaneAssignment col = color_linear_scan(c.problem, bank, false);
            if (is_proper_coloring(c.problem, col, bank, false)) ++proper_ok;

            // Conservador: si el ORIGINAL no spillea, el COALESCIDO tampoco
            // (el coalescing no reintroduce spill).
            LaneAssignment orig = color_linear_scan(p, bank, false);
            if (spill_count(p, orig) == 0) {
                ++no_new_spill_cases;
                if (spill_count(c.problem, col) == 0) ++no_new_spill;
            }
        }
        CHECK(proper_ok == TRIALS,
              "algun coloreado del coalescido fue invalido");
        CHECK(no_new_spill == no_new_spill_cases,
              "el coalescing REINTRODUJO spill (punto 3 fallo)");
        CHECK(did_fuse > 0,
              "el coalescing no fundio NADA en 1000 problemas (no-op?)");
        std::printf("  propio=%d/%d  sin_spill_nuevo=%d/%d  fundieron=%d/%d\n",
                    proper_ok, TRIALS, no_new_spill, no_new_spill_cases,
                    did_fuse, TRIALS);
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
