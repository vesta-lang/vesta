/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_smart_spill.cpp
 * @brief Fase 5 (nucleo): spill INTELIGENTE (victima por coste) vs spill NAIVE.
 *        El coste lo describe el Objective; la estrategia (cost-aware de duracion
 *        restante, NO Belady) es del algoritmo -- separacion dato/mecanismo.  El
 *        Belady real llega con UseDefFacts->next-use.  Property: smart NUNCA peor que
 *        naive + coloreo propio + los valores CALIENTES sobreviven.
 */

#include "codegen/rbank/coloring.h"
#include "codegen/rbank/optimization_context.h"
#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/smart_spill.h"

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
                           ResourceClass cls, ViewWidth w, uint16_t loop_depth = 0) {
    AbstractValue v;
    v.value_id = id;
    v.start = start;
    v.end = end;
    v.req.value_id = id;
    v.req.cls = cls;
    v.req.width = w;
    v.req.loop_depth = loop_depth; // caliente = loop_depth alto.
    return v;
}

int main() {
    std::printf("=== test_rbank_smart_spill (Fase 5: spill por coste) ===\n");

    const BackendCaps caps = [] { BackendCaps c{}; c.sse2 = true; return c; }();
    PhysicalRegisterBank bank = physical_bank_x86_64(true, caps);
    ConstraintSet cs;
    OptimizationContext ctx = make_context(bank, cs);

    const size_t K = bank.allocatable_count(ResourceClass::GP, false);

    // --- Unit: el valor CALIENTE sobrevive; se derrama un FRIO ---
    std::printf("\n[caliente sobrevive, frio se derrama]\n");
    {
        AbstractProblem p;
        // K fríos + 1 caliente, TODOS vivos [0,100] -> K+1 en K lanes -> 1 spill.
        for (uint32_t i = 0; i < K; ++i)
            p.values.push_back(mkval(i + 1, 0, 100, ResourceClass::GP, ViewWidth::W8, 0));
        const uint32_t hot = static_cast<uint32_t>(K) + 1;
        p.values.push_back(mkval(hot, 0, 100, ResourceClass::GP, ViewWidth::W8, 3));

        LaneAssignment s = color_smart_spill(p, ctx, false);
        CHECK(is_proper_coloring(p, s, bank, false), "smart: coloreo invalido");
        CHECK(spill_count(p, s) == 1, "smart: no derramo exactamente 1");
        CHECK(s.lane_of(hot) != kSpilled, "smart derramo el valor CALIENTE (deberia el frio)");
    }

    // --- Unit: sin presion -> 0 spills ---
    std::printf("\n[sin presion -> 0 spills]\n");
    {
        AbstractProblem p;
        p.values = {mkval(1, 0, 5, ResourceClass::GP, ViewWidth::W8),
                    mkval(2, 6, 10, ResourceClass::GP, ViewWidth::W8)};
        LaneAssignment s = color_smart_spill(p, ctx, false);
        CHECK(is_proper_coloring(p, s, bank, false) && spill_count(p, s) == 0,
              "sin presion derramo o coloreo invalido");
    }

    // --- PROPERTY-BASED: smart NUNCA peor que naive + coloreo propio ---
    std::printf("\n[property-based: 1000 problemas, smart vs naive]\n");
    {
        std::mt19937 rng(0x5911u);
        int proper = 0, not_worse = 0, better = 0, pressured = 0;
        const int TRIALS = 1000;
        const double EPS = 1e-6;
        for (int t = 0; t < TRIALS; ++t) {
            std::uniform_int_distribution<uint32_t> nvals(2, 22);
            std::uniform_int_distribution<uint32_t> pos(0, 20);
            std::uniform_int_distribution<uint32_t> dur(0, 30); // rangos largos -> presion.
            std::uniform_int_distribution<uint16_t> depth(0, 3);

            AbstractProblem p;
            const uint32_t n = nvals(rng);
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t s = pos(rng);
                const uint32_t e = s + dur(rng);
                p.values.push_back(mkval(i + 1, s, e, ResourceClass::GP, ViewWidth::W8,
                                         depth(rng)));
            }

            LaneAssignment smart = color_smart_spill(p, ctx, false);
            LaneAssignment naive = color_linear_scan(p, bank, false);

            if (is_proper_coloring(p, smart, bank, false)) ++proper;

            const double cs = total_spill_cost(p, smart, ctx);
            const double cn = total_spill_cost(p, naive, ctx);
            if (cs <= cn + EPS) ++not_worse;
            if (cs < cn - EPS) ++better;
            if (cn > EPS) ++pressured; // hubo spill en el naive.
        }
        CHECK(proper == TRIALS, "smart produjo un coloreado invalido");
        CHECK(not_worse == TRIALS, "smart fue PEOR que el naive en coste de spill");
        CHECK(better > 0, "smart nunca mejoro al naive (no-op?)");
        std::printf("  propio=%d/%d  no_peor=%d/%d  mejoro=%d/%d  (con_presion=%d)\n",
                    proper, TRIALS, not_worse, TRIALS, better, TRIALS, pressured);
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
