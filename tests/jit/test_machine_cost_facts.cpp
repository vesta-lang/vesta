/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_machine_cost_facts.cpp
 * @brief Test de los Facts de HARDWARE (Tipo C): el backend PUBLICA coste
 *        (probe_machine_cost_facts) + autocertificacion + el CostAdapter lo
 *        traduce a los parametros ISA-neutrales del Objective, SIN tocar MInstr
 *        ni cambiar la conducta del path heuristico existente.
 */

#include "analysis/hw/machine_cost_facts.h"
#include "codegen/rbank/adapters/cost_adapter.h"
#include "codegen/rbank/objective.h"
#include "codegen/rbank/value_requirements.h"
#include "jit/machine_ir.h"
#include "jit/sched/cost_model.h"
#include "jit/sched/machine_cost_probe.h"

#include <cstdio>
#include <string>

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

/**
 * @brief Modelo de coste MOCK (solo test): devuelve costes fijos por MOp sin
 *        arrastrar el GenericCostModel/UarchCostModel reales ni las tablas de
 *        arch-data.  Aisla el test al CONTRATO probe -> adapter -> objective:
 *        el probe debe construir LOAD/STORE/MOV y empaquetar lo que el cost
 *        model responda, sea cual sea la microarquitectura.
 */
struct MockCostModel final : jit::sched::SchedCostModel {
    jit::sched::InstrCost cost(const MInstr &mi) const override {
        jit::sched::InstrCost c;
        if (mi.op == MOp::LOAD) {
            c.kind = jit::sched::ExecKind::LOAD;  c.latency = 4.0f; c.recip_tp = 0.5f;
        } else if (mi.op == MOp::STORE) {
            c.kind = jit::sched::ExecKind::STORE; c.latency = 1.0f; c.recip_tp = 1.0f;
        } else { // MOV
            c.kind = jit::sched::ExecKind::ALU;   c.latency = 1.0f; c.recip_tp = 1.0f;
        }
        return c;
    }
    int issue_width() const override { return 4; }
    int port_count() const override { return 1; }
    int port_capacity(int) const override { return 1; }
    const char *port_name(int) const override { return "p0"; }
    const char *name() const override { return "mock-uarch"; }
};

int main() {
    std::printf("=== test_machine_cost_facts (el backend PUBLICA coste HW) ===\n");

    // 1. El backend PUBLICA los Facts del hardware desde el modelo de coste.
    //    (Mock: fija LOAD=4/STORE=1/MOV=1 para verificar el CONTRATO, no la uarch.)
    std::printf("\n[probe: SchedCostModel -> InstrCost -> MachineCostFacts]\n");
    MockCostModel cm;
    analysis::MachineCostFacts hw = jit::sched::probe_machine_cost_facts(cm);
    CHECK(hw.load.kind == jit::sched::ExecKind::LOAD, "load no clasifico como LOAD");
    CHECK(hw.store.kind == jit::sched::ExecKind::STORE, "store no clasifico como STORE");
    CHECK(hw.load.latency == 4.0f, "el probe no empaqueto la latencia de LOAD");
    CHECK(hw.store.latency == 1.0f, "el probe no empaqueto la latencia de STORE");
    CHECK(hw.move.latency == 1.0f, "el probe no empaqueto la latencia de MOV");
    CHECK(hw.from_uarch, "un modelo no-generico debe marcar from_uarch");
    CHECK(std::string(hw.model_name) == "mock-uarch", "model_name mal (trazabilidad)");

    // 2. Autocertificacion: los Facts sanos no reportan; un coste imposible SI.
    std::printf("\n[autocertificacion del Fact de HW]\n");
    CHECK(analysis::validate(hw).empty(), "MachineCostFacts sano reporto");
    {
        analysis::MachineCostFacts bad = hw;
        bad.load.latency = 0.0f; // una op nunca es gratis.
        bool caught = false;
        for (const analysis::MachineCostIssue &i : analysis::validate(bad))
            if (i.check == analysis::MachineCostCheck::LATENCY_NONPOS) caught = true;
        CHECK(caught, "no cazo una latencia <= 0");
    }

    // 3. CostAdapter: traduce el Fact de HW a la card ISA-neutral (sin MInstr).
    std::printf("\n[CostAdapter: MachineCostFacts -> SpillCostCard]\n");
    SpillCostCard card = spill_card_from(hw);
    CHECK(card.reload_latency == 4.0 && card.store_latency == 1.0 &&
              card.move_latency == 1.0 && card.from_hw,
          "card mal traducida del HW");

    // 4. El Objective con card usa el coste REAL del HW; SIN card queda identico.
    std::printf("\n[Objective: HW real (card) vs heuristica (sin card, intacta)]\n");
    ValueRequirements r; r.value_id = 1; r.rematerializable = false;
    const double sin_card = spill_cost_of(r, 1.0);        // fallback heuristico: 3.0
    const double con_card = spill_cost_of(r, 1.0, card);  // HW real: reload 4.0
    CHECK(sin_card == 3.0, "el path SIN card cambio de conducta");
    CHECK(con_card == 4.0, "el path CON card no uso el reload real del HW");

    // 5. Rematerializable con card: recomputar (~1) < recargar.
    ValueRequirements rr; rr.value_id = 2; rr.rematerializable = true;
    CHECK(spill_cost_of(rr, 1.0, card) == 1.0, "remat con card mal");

    // 6. spill_terms con card: spill = reloads*peso; latency = store una vez.
    ObjectiveTerms t = spill_terms(r, 2.0, card);
    CHECK(t.spill == 8.0 && t.latency == 1.0 && t.cache_pressure == 1.0,
          "spill_terms con card mal");

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
