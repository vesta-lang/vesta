/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_shadow.cpp
 * @brief Shadow mode (paso 1): adaptador IntervalResult -> AbstractProblem (extract
 *        facts) + ShadowStats comparables (linear_scan vs rbank).  En AISLAMIENTO:
 *        IntervalResult/RegAlloc sinteticos.  El shadow REAL sobre el corpus es el
 *        cableado en vreg_pipeline (paso 2).
 */

#include "codegen/rbank/optimization_context.h"
#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/shadow.h"

#include <cstdio>

using namespace jit;            // BackendCaps, RegClass, IntervalResult, RegAlloc.
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

static jit::LiveInterval mkiv(uint32_t vreg, jit::RegClass cls, uint32_t from,
                              uint32_t to, int fixed = -1, uint8_t gc = 0) {
    jit::LiveInterval iv;
    iv.vreg = vreg;
    iv.cls = cls;
    iv.fixed_reg = fixed;
    iv.gc_kind = gc;
    iv.ranges.push_back({from, to});
    return iv;
}

static const AbstractValue *find(const AbstractProblem &p, uint32_t id) {
    for (const AbstractValue &v : p.values) if (v.value_id == id) return &v;
    return nullptr;
}

int main() {
    std::printf("=== test_rbank_shadow (adaptador IntervalResult + ShadowStats) ===\n");

    const BackendCaps caps = [] { BackendCaps c{}; c.sse2 = true; return c; }();
    PhysicalRegisterBank bank = physical_bank_x86_64(true, caps);
    ConstraintSet cs;
    OptimizationContext ctx = make_context(bank, cs);

    // --- Adaptador: extrae Facts (envolvente, clase, cross-call, ignora muertos) ---
    std::printf("\n[adaptador: extract facts, ignora vregs muertos]\n");
    {
        jit::IntervalResult ivs;
        ivs.intervals.push_back(mkiv(0, jit::RegClass::GP, 0, 10));   // [0,9] GP
        ivs.intervals.push_back(mkiv(1, jit::RegClass::GP, 5, 15));   // [5,14] GP, cross-call
        ivs.intervals.push_back(mkiv(2, jit::RegClass::FP, 0, 8));    // [0,7] FP
        jit::LiveInterval dead; dead.vreg = 9; dead.cls = jit::RegClass::GP; // ranges vacio
        ivs.intervals.push_back(dead);
        ivs.call_positions = {12}; // solo el vreg 1 [5,15) lo cruza.
        ivs.max_pos = 16;

        AbstractProblem p = intervals_to_problem(ivs);
        CHECK(p.values.size() == 3, "no ignoro el vreg muerto / conteo mal");

        const AbstractValue *v0 = find(p, 0), *v1 = find(p, 1), *v2 = find(p, 2);
        CHECK(v0 && v0->start == 0 && v0->end == 9, "envolvente v0 [0,10)->[0,9] mal");
        CHECK(v1 && v1->start == 5 && v1->end == 14, "envolvente v1 mal");
        CHECK(v0 && v0->req.cls == ResourceClass::GP, "clase v0 (GP) mal");
        CHECK(v2 && v2->req.cls == ResourceClass::FP_VECTOR, "clase v2 (FP) mal");
        CHECK(v1 && v1->req.crosses_call, "v1 deberia cruzar el call en 12");
        CHECK(v0 && !v0->req.crosses_call && v2 && !v2->req.crosses_call,
              "v0/v2 no deberian cruzar el call en 12");
    }

    // --- Pin y GC se propagan ---
    std::printf("\n[pin (fixed_reg) y GC se propagan]\n");
    {
        jit::IntervalResult ivs;
        ivs.intervals.push_back(mkiv(0, jit::RegClass::GP, 0, 5, /*fixed=*/2));
        ivs.intervals.push_back(mkiv(1, jit::RegClass::GP, 6, 10, -1, /*gc=*/1));
        AbstractProblem p = intervals_to_problem(ivs);
        CHECK(find(p, 0)->req.fixed_reg == 2, "pin no propagado");
        CHECK(find(p, 1)->req.is_gc, "gc_kind no propagado a is_gc");
    }

    // --- ShadowStats: linear vs rbank sobre el mismo problema ---
    std::printf("\n[ShadowStats: linear vs rbank]\n");
    {
        jit::IntervalResult ivs;
        // 3 valores GP no-solapan del todo -> banco amplio, 0 spills en rbank.
        ivs.intervals.push_back(mkiv(0, jit::RegClass::GP, 0, 10));
        ivs.intervals.push_back(mkiv(1, jit::RegClass::GP, 3, 12));
        ivs.intervals.push_back(mkiv(2, jit::RegClass::GP, 6, 15));
        ivs.max_pos = 16;
        AbstractProblem p = intervals_to_problem(ivs);

        // RegAlloc sintetico del "linear_scan": pongamos 1 spill artificial (vreg 2).
        jit::RegAlloc ra;
        ra.assign.resize(3);
        ra.assign[0] = {jit::RegAlloc::Loc::REG, 16, 0};
        ra.assign[1] = {jit::RegAlloc::Loc::REG, 17, 0};
        ra.assign[2] = {jit::RegAlloc::Loc::SPILL, 0, 0};

        ShadowStats sl = shadow_stats_linear(ra, p, ctx);
        ShadowStats sr = shadow_stats_rbank(p, ctx, false);

        CHECK(sl.values == 3 && sr.values == 3, "conteo de valores mal");
        CHECK(sl.spills == 1, "linear stats: no conto el spill sintetico");
        CHECK(sr.max_pressure_gp == 3 && sl.max_pressure_gp == 3, "presion GP mal");
        // Banco amplio (>=14 GP): rbank NO derrama -> mejor o igual que el linear.
        CHECK(sr.spills == 0, "rbank derramo con banco amplio");
        CHECK(sr.spills <= sl.spills, "rbank PEOR que linear en spills");
        std::printf("  linear: spills=%u pressure_gp=%u | rbank: spills=%u copies=%u\n",
                    sl.spills, sl.max_pressure_gp, sr.spills, sr.copies_removed);
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
