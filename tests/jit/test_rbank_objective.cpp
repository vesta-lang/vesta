/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_objective.cpp
 * @brief Test de @c Objective (nivel 3, banco ancho): framework de scoring
 *        SUAVE + estimadores.  Verifica ORDENACIONES/propiedades (no valores
 *        exactos): hotness encarece spill, remat lo abarata, cruzar CALL en
 *        lane volatil cuesta mas que en preservada.
 */

#include "jit/rbank/objective.h"
#include "jit/rbank/physical_bank.h"
#include "jit/rbank/value_requirements.h"

#include <cstdio>

using namespace jit;
using namespace jit::rbank;

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

int main() {
    std::printf("=== test_rbank_objective (Fase 0: scoring suave) ===\n");

    const ObjectiveWeights W; // pesos por defecto.

    // --- Framework de scoring ---
    std::printf("\n[framework]\n");
    {
        ObjectiveTerms t; t.spill = 2.0;
        ObjectiveTerms t2; t2.spill = 5.0;
        CHECK(objective_score(t2, W) > objective_score(t, W),
              "mas spill no da mayor coste");
        // Agregacion.
        ObjectiveTerms acc; acc += t; acc += t2;
        CHECK(acc.spill == 7.0, "operator+= no agrega");
    }

    // --- loop_frequency ---
    std::printf("\n[frecuencia por loop_depth]\n");
    CHECK(loop_frequency(0) == 1.0, "depth 0 != 1");
    CHECK(loop_frequency(1) == 10.0, "depth 1 != 10");
    CHECK(loop_frequency(2) == 100.0, "depth 2 != 100");
    CHECK(loop_frequency(20) == loop_frequency(9), "depth no capado en 9");

    // --- spill cost: hotness + rematerializable ---
    std::printf("\n[coste de spill]\n");
    {
        ValueRequirements cold; cold.loop_depth = 0;
        ValueRequirements hot;  hot.loop_depth = 2;
        CHECK(spill_cost_of(hot) > spill_cost_of(cold),
              "spill de valor caliente no cuesta mas");
        ValueRequirements remat; remat.loop_depth = 2; remat.rematerializable = true;
        ValueRequirements plain; plain.loop_depth = 2;
        CHECK(spill_cost_of(remat) < spill_cost_of(plain),
              "rematerializable no abarata el spill");
    }

    // --- callsave: volatil vs preservado ---
    std::printf("\n[coste callsave: volatil vs preservado]\n");
    {
        ValueRequirements nocross; nocross.crosses_call = false;
        CHECK(callsave_cost_of(nocross, SavePolicy::VOLATILE, 5) == 0.0,
              "valor que no cruza call tiene callsave != 0");
        ValueRequirements cross; cross.crosses_call = true;
        const double vol = callsave_cost_of(cross, SavePolicy::VOLATILE, 3);
        const double pres = callsave_cost_of(cross, SavePolicy::PRESERVED, 3);
        CHECK(vol > pres, "volatil con 3 calls no cuesta mas que preservado");
        CHECK(pres == 2.0, "preservado no es coste unico (2)");
    }

    // --- lane_choice_terms: preferir callee-saved para valores que cruzan CALL ---
    std::printf("\n[eleccion de lane: crossing -> preferir preservado]\n");
    {
        const BackendCaps caps = [] { BackendCaps c{}; c.sse2 = true; return c; }();
        PhysicalRegisterBank a64 = physical_bank_arm64(caps);
        const Lane *v0 = a64.by_id(32); // caller-saved (VOLATILE)
        const Lane *v8 = a64.by_id(40); // callee-saved (PRESERVED)
        CHECK(v0 && v8, "lanes arm64 no encontradas");
        ValueRequirements r; r.cls = ResourceClass::FP_VECTOR; r.width = ViewWidth::W8;
        r.crosses_call = true;
        const ObjectiveTerms tv = lane_choice_terms(r, *v0, 4); // volatil, 4 calls
        const ObjectiveTerms tp = lane_choice_terms(r, *v8, 4); // preservada
        CHECK(objective_score(tv, W) > objective_score(tp, W),
              "elegir lane volatil para valor que cruza call no cuesta mas");
        CHECK(tv.register_pressure == 1.0 && tp.register_pressure == 1.0,
              "register_pressure de una lane no es 1");
    }

    // --- spill_terms vs mantener en registro (valor caliente) ---
    std::printf("\n[hot value: registro mejor que spill]\n");
    {
        const BackendCaps caps = [] { BackendCaps c{}; c.sse2 = c.avx = true; return c; }();
        PhysicalRegisterBank sysv = physical_bank_x86_64(true, caps);
        const Lane *xmm0 = sysv.by_id(16); // volatil, no cruza call aqui
        ValueRequirements r; r.cls = ResourceClass::FP_VECTOR; r.width = ViewWidth::W8;
        r.loop_depth = 3; // muy caliente
        const ObjectiveTerms keep = lane_choice_terms(r, *xmm0, 0);
        const ObjectiveTerms spill = spill_terms(r);
        CHECK(objective_score(spill, W) > objective_score(keep, W),
              "para un valor caliente, spill deberia costar mas que registro");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
