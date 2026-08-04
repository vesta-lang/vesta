/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_fragmentation_recovery.cpp
 * @brief Splitting incremento 3c: la @c FragmentationRecovery produce planes REALES.
 *
 * Se prueba el ALGORITMO aislado (sin compilar nada): un problema sintetico donde todas
 * las lanes menos una estan ocupadas siempre, y esa una tiene un HUECO.  Se comprueba que
 * el plan (a) aprovecha el hueco, (b) respeta las condiciones de correctitud -- no cruza
 * bloques, deja base antes y despues -- y (c) obedece al cost model.
 */

#include "codegen/rbank/fragmentation_recovery.h"
#include "codegen/rbank/lane_occupancy.h"
#include "jit/backend_caps.h"
#include "jit/target_reginfo.h"

#include <cstdio>
#include <vector>

using namespace codegen;
using namespace codegen::rbank;

static int g_checks = 0, g_fails = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(c)) {                                                              \
            ++g_fails;                                                           \
            std::printf("  FALLO L%d: %s\n", __LINE__, #c);                      \
        }                                                                        \
    } while (0)

namespace {

/// Escenario sintetico: N-1 lanes GP ocupadas SIEMPRE y una (la primera admisible) libre
/// solo en [hole_from, hole_to].  El valor 0 esta derramado y vive en [1, life_to].
struct Scenario {
    PhysicalRegisterBank bank;
    AbstractProblem      p;
    LaneAssignment       la;
    jit::IntervalResult  ivs;
    uint8_t              free_lane = 0;
};

Scenario make_scenario(uint32_t hole_from, uint32_t hole_to, uint32_t life_to,
                       const std::vector<uint32_t> &uses,
                       const std::vector<uint32_t> &block_starts) {
    Scenario s;
    s.bank = physical_bank_x86_64_from_reginfo(jit::target_x86_64_vm_abi(),
                                               jit::backend_caps_host());

    // El valor derramado: GP normal, sin pin, sin cruzar CALL.
    AbstractValue v;
    v.value_id = 0;
    v.start = 1;
    v.end = life_to;
    v.req.value_id = 0;
    v.req.cls = ResourceClass::GP;
    v.req.width = ViewWidth::W8;
    v.req.fixed_reg = -1;
    s.p.values.push_back(v);
    s.la.spill(0);

    // Bloqueadores: uno por lane admisible.  La PRIMERA deja el hueco; el resto ocupan
    // toda la ventana temporal -> el valor 0 es `partially` (hay hueco, pero no completo).
    uint32_t next_id = 1;
    bool first = true;
    for (const Lane &l : s.bank.lanes) {
        if (!lane_admissible(v.req, l, /*vec_active*/ false)) continue;
        auto blocker = [&](uint32_t from, uint32_t to) {
            AbstractValue b;
            b.value_id = next_id;
            b.start = from;
            b.end = to;
            b.req.value_id = next_id;
            b.req.cls = ResourceClass::GP;
            b.req.width = ViewWidth::W8;
            b.req.fixed_reg = -1;
            s.p.values.push_back(b);
            s.la.assign(next_id, l.id);
            ++next_id;
        };
        if (first) {
            s.free_lane = l.id;
            first = false;
            if (hole_from > 0) blocker(0, hole_from - 1);
            blocker(hole_to + 1, 400);
        } else {
            blocker(0, 400);
        }
    }

    // Intervalos reales del backend: solo el valor 0 nos interesa (los bloqueadores ya
    // estan en registro y el algoritmo ni los mira).
    s.ivs.intervals.resize(next_id);
    for (uint32_t i = 0; i < next_id; ++i) s.ivs.intervals[i].vreg = i;
    s.ivs.intervals[0].ranges.push_back({1, life_to + 1}); // [from,to) semiabierto.
    s.ivs.intervals[0].uses = uses;
    s.ivs.max_pos = 402;
    s.ivs.block_starts = block_starts;
    return s;
}

} // namespace

int main() {
    std::printf("=== test_fragmentation_recovery (Splitting incremento 3c) ===\n");

    /* --- 1. Caso nominal: hueco [9,19], 5 usos dentro, un solo bloque --- */
    {
        Scenario s = make_scenario(9, 19, 30, {10, 12, 14, 16, 18}, {0});
        FragmentationStats st;
        const RecoveryCostModel cost;
        const AssignmentPlan plan =
            build_fragmentation_plan(s.p, s.la, s.bank, false, s.ivs, cost, &st);

        CHECK(plan.intervals.size() == 1);
        if (plan.intervals.size() == 1) {
            const AssignmentInterval &si = plan.intervals[0];
            CHECK(si.vreg == 0);
            CHECK(si.from.value == 10);      // primer uso del hueco.
            CHECK(si.to.value == 18 + 2);    // ultimo uso + def-point de esa instr.
            CHECK(si.location.is_register());
            CHECK(si.location.register_id() == s.free_lane); // la UNICA lane con hueco.
        }
        CHECK(st.values_split == 1);
        CHECK(st.intervals == 1);
        CHECK(st.uses_recovered == 5);
        CHECK(st.recovered_area == 10); // [10,20)
    }

    /* --- 2. Cost model: un solo uso NO compensa (1 uso < carga + descarga) --- */
    {
        Scenario s = make_scenario(9, 19, 30, {12}, {0});
        FragmentationStats st;
        const RecoveryCostModel cost;
        const AssignmentPlan plan =
            build_fragmentation_plan(s.p, s.la, s.bank, false, s.ivs, cost, &st);
        CHECK(plan.intervals.empty());
        CHECK(st.rejected_cost >= 1);  // hubo hueco, pero no compensaba.
        CHECK(st.recovered_area == 0);
    }

    /* --- 3. Condicion 1 (RECTILINEO): una frontera de bloque en 14 parte el hueco.
     *        El tramo NO puede cruzarla, asi que el plan empieza en 14 (no en 10) y solo
     *        captura los usos del segundo bloque. --- */
    {
        Scenario s = make_scenario(9, 19, 30, {10, 12, 14, 16, 18}, {0, 14});
        FragmentationStats st;
        const RecoveryCostModel cost;
        const AssignmentPlan plan =
            build_fragmentation_plan(s.p, s.la, s.bank, false, s.ivs, cost, &st);
        CHECK(plan.intervals.size() == 1);
        if (plan.intervals.size() == 1) {
            CHECK(plan.intervals[0].from.value == 14); // no cruza la frontera.
            CHECK(plan.intervals[0].to.value == 20);
        }
        CHECK(st.uses_recovered == 3); // 14, 16, 18 (los de antes quedan fuera).
    }

    /* --- 4. Condicion 3 (VUELVE A LA BASE): el valor muere en 19, asi que el tramo NO
     *        puede llegar hasta el ultimo uso -- necesitaria to=20 y ahi ya no hay tramo
     *        base en memoria donde devolverlo.  CONTRATO (no "lo que salga"): el
     *        algoritmo recorta hasta el penultimo uso y recupera [10,18) con 4 usos.  Si
     *        una heuristica futura dejara de recuperar este caso, este test lo detecta;
     *        por eso se fija el resultado exacto y no solo "to < 20". --- */
    {
        Scenario s = make_scenario(9, 19, 19, {10, 12, 14, 16, 18}, {0});
        FragmentationStats st;
        const RecoveryCostModel cost;
        const AssignmentPlan plan =
            build_fragmentation_plan(s.p, s.la, s.bank, false, s.ivs, cost, &st);
        CHECK(plan.intervals.size() == 1);
        if (plan.intervals.size() == 1) {
            CHECK(plan.intervals[0].from.value == 10);
            CHECK(plan.intervals[0].to.value == 18); // recortado: 20 seria el fin de la vida.
        }
        CHECK(st.uses_recovered == 4); // el uso 18 queda fuera.
    }

    /* --- 5. Valor que DEBE vivir en memoria (GC root cross-call): jamas se planifica --- */
    {
        Scenario s = make_scenario(9, 19, 30, {10, 12, 14, 16, 18}, {0});
        s.p.values[0].req.residency = Residency::MEMORY;
        FragmentationStats st;
        const RecoveryCostModel cost;
        const AssignmentPlan plan =
            build_fragmentation_plan(s.p, s.la, s.bank, false, s.ivs, cost, &st);
        CHECK(plan.intervals.empty());
    }

    /* --- 6. Sin usos no hay nada que ahorrar: plan vacio (y sin tocar estadisticas) --- */
    {
        Scenario s = make_scenario(9, 19, 30, {}, {0});
        FragmentationStats st;
        const RecoveryCostModel cost;
        const AssignmentPlan plan =
            build_fragmentation_plan(s.p, s.la, s.bank, false, s.ivs, cost, &st);
        CHECK(plan.intervals.empty());
        CHECK(st.values_split == 0);
    }

    /* @c LaneOccupancy (invariante de normalizacion + aliasing) NO se prueba aqui: es un
     * componente independiente con sus propios consumidores -> tests/jit/test_lane_occupancy.cpp. */

    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
