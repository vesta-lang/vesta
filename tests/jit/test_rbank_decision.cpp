/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_decision.cpp
 * @brief Test de @c OptimizationContext + @c DecisionPolicy (cuspide del modelo
 *        de banco ancho): Budget (topes duros), ensamblaje del contexto y la
 *        seleccion determinista (menor coste, empate estable, pesos cambian la
 *        eleccion, explicacion como DATO i18n-ready).
 */

#include "codegen/rbank/constraints.h"
#include "codegen/rbank/decision_policy.h"
#include "codegen/rbank/objective.h"
#include "codegen/rbank/optimization_context.h"
#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/value_requirements.h"

#include <cstdio>
#include <vector>

using namespace jit;
using namespace codegen::rbank;

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

static Candidate cand(uint32_t id, ObjectiveTerms t, bool valid = true) {
    Candidate c;
    c.id = handle(id, DecisionKind::LANE);
    c.terms = t;
    c.valid = valid;
    return c;
}

int main() {
    std::printf("=== test_rbank_decision (Fase 0: contexto + politica) ===\n");

    const BackendCaps caps = [] {
        BackendCaps c{};
        c.sse2 = c.avx = true;
        return c;
    }();
    PhysicalRegisterBank bank = physical_bank_x86_64(true, caps);
    ConstraintSet cs;

    // --- Budget: topes duros ---
    std::printf("\n[budget: topes duros]\n");
    {
        Budget b; // por defecto ilimitado
        CHECK(b.unlimited_compile() && !b.compile_exceeded(1'000'000'000ull),
              "budget por defecto no ilimitado");
        b.compile_ns = 1000;
        CHECK(!b.compile_exceeded(500) && b.compile_exceeded(2000),
              "compile_exceeded incorrecto");
        b.memory_bytes = 4096;
        CHECK(!b.memory_exceeded(1000) && b.memory_exceeded(8192),
              "memory_exceeded incorrecto");
    }

    // --- Context: ensamblaje + accessors ---
    std::printf("\n[contexto: ensamblaje]\n");
    {
        OptimizationContext ctx = make_context(bank, cs);
        CHECK(ctx.has_bank() && ctx.has_constraints(),
              "contexto sin bank/constraints");
        CHECK(&ctx.get_bank() == &bank, "get_bank no apunta al banco");
        ObjectiveTerms t;
        t.spill = 1.0;
        CHECK(ctx.score(t) == objective_score(t, ctx.weights()),
              "score del contexto != objective_score");
    }

    WeightedObjectivePolicy policy;

    // --- Seleccion: menor coste valido ---
    std::printf("\n[seleccion: menor coste]\n");
    {
        OptimizationContext ctx = make_context(bank, cs);
        ObjectiveTerms cheap;
        cheap.register_pressure = 1.0; // 0.25 con peso default
        ObjectiveTerms dear;
        dear.spill = 2.0; // 8.0 con peso default
        std::vector<Candidate> cands = {cand(0, cheap), cand(1, dear)};
        DecisionExplanation ex;
        const Candidate *best = policy.choose(cands, ctx, &ex);
        CHECK(best && best->id.value == 0, "no eligio el candidato mas barato");
        CHECK(ex.found && ex.chosen.value == 0 &&
                  ex.candidates_considered == 2 && ex.candidates_rejected == 0,
              "explicacion incorrecta");
    }

    // --- Candidatos invalidos descartados ---
    std::printf("\n[candidatos invalidos]\n");
    {
        OptimizationContext ctx = make_context(bank, cs);
        ObjectiveTerms cheap;
        cheap.register_pressure = 1.0;
        ObjectiveTerms cheaper;
        cheaper.register_pressure = 0.1;
        // el mas barato es invalido -> se descarta.
        std::vector<Candidate> cands = {cand(0, cheap),
                                        cand(1, cheaper, /*valid=*/false)};
        DecisionExplanation ex;
        const Candidate *best = policy.choose(cands, ctx, &ex);
        CHECK(best && best->id.value == 0, "no descarto el candidato invalido");
        CHECK(ex.candidates_rejected == 1, "no conto el invalido");
    }
    {
        // ninguno valido -> nullptr.
        OptimizationContext ctx = make_context(bank, cs);
        std::vector<Candidate> cands = {cand(0, {}, false), cand(1, {}, false)};
        DecisionExplanation ex;
        const Candidate *best = policy.choose(cands, ctx, &ex);
        CHECK(!best && !ex.found && ex.candidates_rejected == 2,
              "sin candidatos validos no da nullptr/found=false");
    }

    // --- Empate estable (menor id / orden de entrada) ---
    std::printf("\n[empate estable]\n");
    {
        OptimizationContext ctx = make_context(bank, cs);
        ObjectiveTerms t;
        t.register_pressure = 1.0;
        std::vector<Candidate> cands = {cand(5, t), cand(3, t)}; // mismo score
        const Candidate *best = policy.choose(cands, ctx);
        CHECK(best && best->id.value == 5,
              "empate no estable (deberia ganar el primero)");
    }

    // --- Los pesos cambian la eleccion ---
    std::printf("\n[pesos del contexto cambian el ganador]\n");
    {
        ObjectiveTerms a;
        a.callsave = 3.0; // default: 2*3 = 6
        ObjectiveTerms b;
        b.spill = 2.0; // default: 4*2 = 8
        std::vector<Candidate> cands = {cand(0, a), cand(1, b)};
        // Default: gana A (6 < 8).
        OptimizationContext def = make_context(bank, cs);
        CHECK(policy.choose(cands, def)->id.value == 0,
              "con pesos default no gana A");
        // callsave carisimo -> gana B.
        ObjectiveWeights w;
        w.callsave = 100.0;
        OptimizationContext ctx = make_context(bank, cs, w);
        const Candidate *best = policy.choose(cands, ctx);
        CHECK(best && best->id.value == 1, "con callsave carisimo no gana B");
    }

    // --- Explicacion: dominante + top contribuyentes como DATO ---
    std::printf("\n[explicacion: dominante + top-N]\n");
    {
        OptimizationContext ctx = make_context(bank, cs);
        // spill=2 (8.0), callsave=1 (2.0), register_pressure=1 (0.25) ->
        // total 10.25.
        ObjectiveTerms t;
        t.spill = 2.0;
        t.callsave = 1.0;
        t.register_pressure = 1.0;
        std::vector<Candidate> cands = {cand(0, t)};
        DecisionExplanation ex;
        policy.choose(cands, ctx, &ex);
        CHECK(ex.dominant == DominantTerm::SPILL,
              "termino dominante no es SPILL");
        // handle tipado preservado en la explicacion.
        CHECK(ex.chosen.kind == DecisionKind::LANE,
              "handle no conserva el kind LANE");
        // top-3: spill primero, luego callsave, luego register_pressure.
        CHECK(ex.top_count == 3, "top_count != 3");
        CHECK(ex.top[0].term == DominantTerm::SPILL && ex.top[0].fraction > 0.7,
              "top[0] no es SPILL con fraccion dominante");
        CHECK(ex.top[1].term == DominantTerm::CALLSAVE,
              "top[1] no es CALLSAVE");
        CHECK(ex.top[2].term == DominantTerm::REGISTER_PRESSURE,
              "top[2] no es REGISTER_PRESSURE");
        // las fracciones suman ~1.
        const double sum =
            ex.top[0].fraction + ex.top[1].fraction + ex.top[2].fraction;
        CHECK(sum > 0.99 && sum < 1.01, "las fracciones del top no suman ~1");
    }

    // --- Convergencia programa + hardware en el contexto (Fase 0.25) ---
    std::printf(
        "\n[convergencia: programa (exec_weight) + hardware (hw_cost)]\n");
    {
        // Fact del PROGRAMA: valor f64 en loop_depth=2 (exec_weight
        // estatico=100).
        ValueRequirements r;
        r.value_id = 1;
        r.cls = ResourceClass::FP_VECTOR;
        r.loop_depth = 2;
        r.rematerializable = false;

        // (a) Sin HW cableado -> fallback generico (reload=4.0), from_hw=false.
        OptimizationContext ctx_sw = make_context(bank, cs);
        CHECK(!ctx_sw.has_hw_cost(), "contexto sin HW marca from_hw");
        ObjectiveTerms t_sw = ctx_sw.spill_terms_for(r);
        // spill = reload(4.0 fallback) x exec_weight(100) = 400.
        CHECK(t_sw.spill == 400.0, "convergencia fallback mal (4*100)");

        // (b) Con HW real (uarch con reload caro=10) -> el HW escala el spill.
        SpillCostCard hw;
        hw.reload_latency = 10.0;
        hw.store_latency = 2.0;
        hw.move_latency = 1.0;
        hw.from_hw = true;
        OptimizationContext ctx_hw = make_context(bank, cs, {}, {}, false, hw);
        CHECK(ctx_hw.has_hw_cost(), "contexto con HW no lo marca");
        ObjectiveTerms t_hw = ctx_hw.spill_terms_for(r);
        // spill = reload(10) x exec_weight(100) = 1000; latency = store(2).
        CHECK(t_hw.spill == 1000.0 && t_hw.latency == 2.0,
              "el HW real no se propago al spill (10*100) / store");

        // (c) El perfil MEDIDO del programa (PGO) manda sobre el estatico.
        ValueRequirements rp = r;
        rp.execution_weight = 5000.0;
        ObjectiveTerms t_pgo = ctx_hw.spill_terms_for(rp);
        // spill = reload(10) x exec_weight_medido(5000) = 50000.
        CHECK(t_pgo.spill == 50000.0, "el perfil medido no domino (10*5000)");

        // (d) La DECISION refleja la convergencia: derramar el valor caliente
        // en
        //     la uarch cara es peor que ocupar una lane -> el motor evita el
        //     spill.
        ObjectiveTerms lane_t;
        lane_t.register_pressure = 1.0; // barato
        std::vector<Candidate> cands = {cand(0, lane_t), cand(1, t_hw)};
        const Candidate *best = policy.choose(cands, ctx_hw);
        CHECK(best && best->id.value == 0, "el motor no evito el spill caro");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
