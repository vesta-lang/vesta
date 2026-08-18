/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_shadow.cpp
 * @brief Shadow mode (paso 1): adaptador IntervalResult -> AbstractProblem
 * (extract facts) + ShadowStats comparables (linear_scan vs rbank).  En
 * AISLAMIENTO: IntervalResult/codegen::RegAlloc sinteticos.  El shadow REAL
 * sobre el corpus es el cableado en vreg_pipeline (paso 2).
 */

#include "codegen/rbank/optimization_context.h"
#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/shadow.h"

#include <cstdio>

using namespace jit;            // BackendCaps, RegClass, IntervalResult,
                                // codegen::RegAlloc.
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
    for (const AbstractValue &v : p.values)
        if (v.value_id == id) return &v;
    return nullptr;
}

int main() {
    std::printf(
        "=== test_rbank_shadow (adaptador IntervalResult + ShadowStats) ===\n");

    const BackendCaps caps = [] {
        BackendCaps c{};
        c.sse2 = true;
        return c;
    }();
    PhysicalRegisterBank bank = physical_bank_x86_64(true, caps);
    ConstraintSet cs;
    OptimizationContext ctx = make_context(bank, cs);

    // --- Adaptador: extrae Facts (envolvente, clase, cross-call, ignora
    // muertos) ---
    std::printf("\n[adaptador: extract facts, ignora vregs muertos]\n");
    {
        jit::IntervalResult ivs;
        ivs.intervals.push_back(mkiv(0, jit::RegClass::GP, 0, 10)); // [0,9] GP
        ivs.intervals.push_back(
            mkiv(1, jit::RegClass::GP, 5, 15)); // [5,14] GP, cross-call
        ivs.intervals.push_back(mkiv(2, jit::RegClass::FP, 0, 8)); // [0,7] FP
        jit::LiveInterval dead;
        dead.vreg = 9;
        dead.cls = jit::RegClass::GP; // ranges vacio
        ivs.intervals.push_back(dead);
        ivs.call_positions = {12}; // solo el vreg 1 [5,15) lo cruza.
        ivs.max_pos = 16;

        AbstractProblem p = intervals_to_problem(ivs);
        CHECK(p.values.size() == 3, "no ignoro el vreg muerto / conteo mal");

        const AbstractValue *v0 = find(p, 0), *v1 = find(p, 1),
                            *v2 = find(p, 2);
        CHECK(v0 && v0->start == 0 && v0->end == 9,
              "envolvente v0 [0,10)->[0,9] mal");
        CHECK(v1 && v1->start == 5 && v1->end == 14, "envolvente v1 mal");
        CHECK(v0 && v0->req.cls == ResourceClass::GP, "clase v0 (GP) mal");
        CHECK(v2 && v2->req.cls == ResourceClass::FP_VECTOR,
              "clase v2 (FP) mal");
        CHECK(v1 && v1->req.crosses_call, "v1 deberia cruzar el call en 12");
        CHECK(v0 && !v0->req.crosses_call && v2 && !v2->req.crosses_call,
              "v0/v2 no deberian cruzar el call en 12");
    }

    // --- Pin y GC se propagan ---
    std::printf("\n[pin (fixed_reg) y GC se propagan]\n");
    {
        jit::IntervalResult ivs;
        ivs.intervals.push_back(mkiv(0, jit::RegClass::GP, 0, 5, /*fixed=*/2));
        ivs.intervals.push_back(
            mkiv(1, jit::RegClass::GP, 6, 10, -1, /*gc=*/1));
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

        // codegen::RegAlloc sintetico del "linear_scan": pongamos 1 spill
        // artificial (vreg 2).
        codegen::RegAlloc ra;
        ra.assign.resize(3);
        ra.assign[0] = {codegen::RegAlloc::Loc::REG, 16, 0};
        ra.assign[1] = {codegen::RegAlloc::Loc::REG, 17, 0};
        ra.assign[2] = {codegen::RegAlloc::Loc::SPILL, 0, 0};

        ShadowStats sl = shadow_stats_linear(ra, p, ctx);
        ShadowStats sr = shadow_stats_rbank(p, ctx, false);

        CHECK(sl.values == 3 && sr.values == 3, "conteo de valores mal");
        CHECK(sl.spills == 1, "linear stats: no conto el spill sintetico");
        CHECK(sr.max_pressure_gp == 3 && sl.max_pressure_gp == 3,
              "presion GP mal");
        // Banco amplio (>=14 GP): rbank NO derrama -> mejor o igual que el
        // linear.
        CHECK(sr.spills == 0, "rbank derramo con banco amplio");
        CHECK(sr.spills <= sl.spills, "rbank PEOR que linear en spills");
        // allocated: linear 2 en reg + 1 spilled; rbank 3 en reg.
        CHECK(sl.allocated_gp == 2, "linear allocated_gp != 2");
        CHECK(sr.allocated_gp == 3, "rbank allocated_gp != 3");
        // ShadowDiff (rbank - linear): -1 spill, +1 registro usado.
        ShadowDiff d = shadow_diff(sl, sr);
        CHECK(d.spills_delta == -1, "diff spills != -1 (rbank mejora)");
        CHECK(d.allocated_gp_delta == 1, "diff allocated_gp != +1");
        std::printf("  linear: spills=%u alloc_gp=%u | rbank: spills=%u "
                    "alloc_gp=%u copies=%u"
                    " | DIFF spills=%+d cost=%+.1f\n",
                    sl.spills, sl.allocated_gp, sr.spills, sr.allocated_gp,
                    sr.copies_removed, d.spills_delta, d.spill_cost_delta);
    }

    // --- pinned_values en las stats ---
    std::printf("\n[pinned_values contado en stats]\n");
    {
        jit::IntervalResult ivs;
        ivs.intervals.push_back(mkiv(0, jit::RegClass::GP, 0, 5, /*fixed=*/2));
        ivs.intervals.push_back(mkiv(1, jit::RegClass::GP, 6, 10));
        AbstractProblem p = intervals_to_problem(ivs);
        ShadowStats sr = shadow_stats_rbank(p, ctx, false);
        CHECK(sr.pinned_values == 1, "pinned_values != 1");
    }

    // --- ShadowReport (veredicto por funcion) + ShadowAggregate (corpus) ---
    std::printf("\n[ShadowReport + ShadowAggregate: panel de calidad]\n");
    {
        ShadowStats lin1;
        lin1.spills = 5;
        lin1.spill_cost = 10.0; // rbank mejora
        ShadowStats rb1;
        rb1.spills = 3;
        rb1.spill_cost = 6.0;
        ShadowReport r1 = make_shadow_report(lin1, rb1);
        CHECK(r1.improved && !r1.equivalent, "r1 deberia MEJORAR");

        ShadowStats lin2;
        lin2.spills = 2;
        lin2.spill_cost = 4.0; // equivalente
        ShadowStats rb2;
        rb2.spills = 2;
        rb2.spill_cost = 4.0;
        ShadowReport r2 = make_shadow_report(lin2, rb2);
        CHECK(r2.equivalent && !r2.improved, "r2 deberia ser EQUIVALENTE");

        ShadowStats lin3;
        lin3.spills = 1;
        lin3.spill_cost = 2.0; // rbank empeora
        ShadowStats rb3;
        rb3.spills = 3;
        rb3.spill_cost = 6.0;
        ShadowReport r3 = make_shadow_report(lin3, rb3);
        CHECK(!r3.improved && !r3.equivalent, "r3 deberia EMPEORAR");

        ShadowAggregate agg;
        agg.add(r1);
        agg.add(r2);
        agg.add(r3);
        CHECK(agg.functions == 3, "agg.functions != 3");
        CHECK(agg.improved == 1 && agg.equal == 1 && agg.worsened == 1,
              "agg veredictos mal");
        CHECK(agg.linear_spills == 8 && agg.rbank_spills == 8,
              "agg spills totales mal");
        std::printf("  funcs=%u iguales=%u mejoran=%u empeoran=%u | spills "
                    "lin=%llu rbank=%llu\n",
                    agg.functions, agg.equal, agg.improved, agg.worsened,
                    (unsigned long long)agg.linear_spills,
                    (unsigned long long)agg.rbank_spills);
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
