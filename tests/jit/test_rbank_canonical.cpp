/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_canonical.cpp
 * @brief Fase 1: canonicalizacion + cache de soluciones.  El allocator recibe
 * un problema CANONICO -> el mismo problema (salvo etiquetas/offset/orden) se
 *        colorea UNA vez y se REUSA.
 *
 * Propiedades:
 *   1. canonical_hash es INVARIANTE bajo (renombrar value_ids + trasladar
 *      posiciones + reordenar la lista).
 *   2. Problemas estructuralmente DISTINTOS dan hashes distintos.
 *   3. La cache: colorear una forma canonica y REUSARLA para un problema
 *      equivalente (HIT) produce un coloreado propio identico al directo.
 */

#include "codegen/rbank/canonical_problem.h"
#include "codegen/rbank/coloring.h"
#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/solution_cache.h"

#include <cstdio>
#include <numeric>
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

/** @brief Problema equivalente: renombra ids + traslada posiciones + baraja. */
static AbstractProblem transform_equivalent(const AbstractProblem &p,
                                            std::mt19937 &rng) {
    // Permutacion de value_ids (biyeccion sobre 100..).
    std::vector<uint32_t> new_ids(p.values.size());
    std::iota(new_ids.begin(), new_ids.end(), 100u);
    std::shuffle(new_ids.begin(), new_ids.end(), rng);
    const uint32_t off = std::uniform_int_distribution<uint32_t>(1, 500)(rng);

    AbstractProblem q;
    for (size_t i = 0; i < p.values.size(); ++i) {
        AbstractValue av = p.values[i];
        av.value_id = new_ids[i];
        av.req.value_id = new_ids[i];
        av.start +=
            off; // traslacion (no cambia la estructura de solapamiento).
        av.end += off;
        q.values.push_back(av);
    }
    std::shuffle(q.values.begin(), q.values.end(), rng); // reordenar la lista.
    return q;
}

int main() {
    std::printf(
        "=== test_rbank_canonical (Fase 1: forma canonica + cache) ===\n");

    const BackendCaps caps = [] {
        BackendCaps c{};
        c.sse2 = true;
        return c;
    }();
    PhysicalRegisterBank bank = physical_bank_x86_64(true, caps);

    // --- Unit: invariancia bajo traslacion + permutacion de ids ---
    std::printf("\n[hash invariante bajo traslacion/permutacion/orden]\n");
    {
        AbstractProblem p;
        p.values = {mkval(1, 0, 10, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 5, 15, ResourceClass::GP, ViewWidth::W8),
                    mkval(3, 12, 20, ResourceClass::FP_VECTOR, ViewWidth::W16)};
        // Trasladado +1000 con ids distintos y en otro orden.
        AbstractProblem q;
        q.values = {
            mkval(77, 1012, 1020, ResourceClass::FP_VECTOR, ViewWidth::W16),
            mkval(55, 1000, 1010, ResourceClass::GP, ViewWidth::W8),
            mkval(66, 1005, 1015, ResourceClass::GP, ViewWidth::W8)};
        CHECK(canonical_hash(p) == canonical_hash(q),
              "hash NO invariante bajo traslacion/permutacion/orden");
    }

    // --- Unit: problemas estructuralmente distintos -> hash distinto ---
    std::printf("\n[estructura distinta -> hash distinto]\n");
    {
        AbstractProblem a;
        a.values = {mkval(1, 0, 10, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 5, 15, ResourceClass::GP, ViewWidth::W8)};
        AbstractProblem b; // el 2o valor NO se solapa con el 1o.
        b.values = {mkval(1, 0, 10, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 20, 30, ResourceClass::GP, ViewWidth::W8)};
        CHECK(canonical_hash(a) != canonical_hash(b),
              "estructuras distintas colisionaron el hash");
        AbstractProblem c; // misma estructura que 'a' pero clase distinta.
        c.values = {mkval(1, 0, 10, ResourceClass::FP_VECTOR, ViewWidth::W16),
                    mkval(2, 5, 15, ResourceClass::FP_VECTOR, ViewWidth::W16)};
        CHECK(canonical_hash(a) != canonical_hash(c),
              "clase distinta no cambio el hash");
    }

    // --- Unit: cache HIT reusa la solucion de la forma equivalente ---
    std::printf("\n[cache: forma equivalente -> HIT + coloreado propio]\n");
    {
        AbstractProblem p;
        p.values = {mkval(1, 0, 10, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 0, 10, ResourceClass::GP, ViewWidth::W8),
                    mkval(3, 0, 10, ResourceClass::GP, ViewWidth::W8)};
        std::mt19937 rng(1u);
        AbstractProblem q = transform_equivalent(p, rng);

        SolutionCache cache;
        LaneAssignment s1 = cache.solve(p, bank, false); // miss
        LaneAssignment s2 = cache.solve(q, bank, false); // hit (misma forma)
        CHECK(cache.misses == 1 && cache.hits == 1,
              "la forma equivalente no dio HIT");
        CHECK(is_proper_coloring(p, s1, bank, false),
              "solve(p) no es coloreado propio");
        CHECK(is_proper_coloring(q, s2, bank, false),
              "solve(q) no es coloreado propio");
        // La solucion via cache == coloreado directo (mismo problema).
        LaneAssignment direct = color_linear_scan(p, bank, false);
        bool same = true;
        for (const AbstractValue &v : p.values)
            if (direct.lane_of(v.value_id) != s1.lane_of(v.value_id))
                same = false;
        CHECK(same, "la solucion cacheada difiere del coloreado directo");
    }

    // --- PROPERTY-BASED: invariancia + HIT sobre CFGs sinteticos ---
    std::printf("\n[property-based: 1000 problemas + equivalentes]\n");
    {
        std::mt19937 rng(0xBEEFu);
        int hash_ok = 0, hit_ok = 0, proper_ok = 0;
        const int TRIALS = 1000;
        for (int t = 0; t < TRIALS; ++t) {
            std::uniform_int_distribution<uint32_t> nvals(1, 20);
            std::uniform_int_distribution<uint32_t> pos(0, 30);
            std::uniform_int_distribution<uint32_t> dur(0, 10);
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
            AbstractProblem q = transform_equivalent(p, rng);

            if (canonical_hash(p) == canonical_hash(q)) ++hash_ok;

            SolutionCache cache;
            LaneAssignment s1 = cache.solve(p, bank, false);
            LaneAssignment s2 = cache.solve(q, bank, false);
            if (cache.hits == 1 && cache.misses == 1) ++hit_ok;
            if (is_proper_coloring(p, s1, bank, false) &&
                is_proper_coloring(q, s2, bank, false))
                ++proper_ok;
        }
        CHECK(hash_ok == TRIALS, "hash no invariante en algun equivalente");
        CHECK(hit_ok == TRIALS, "la forma equivalente no siempre dio HIT");
        CHECK(proper_ok == TRIALS, "algun coloreado via cache fue invalido");
        std::printf("  hash_invariante=%d/%d  cache_hit=%d/%d  propio=%d/%d\n",
                    hash_ok, TRIALS, hit_ok, TRIALS, proper_ok, TRIALS);
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
