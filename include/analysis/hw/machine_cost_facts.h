/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/hw/machine_cost_facts.h
 * @brief MachineCostFacts: Fact de TIPO C -- describe el HARDWARE, no el
 * programa.
 *
 * Aparece una tercera categoria de Facts junto a las dos ya existentes:
 *
 *      Tipo A (analysis/facts/)    describen el PROGRAMA desde el IR/CFG:
 *                                  LoopFacts, DomFacts, Liveness, Alias.
 *      Tipo B (analysis/derived/)  DERIVADOS que combinan productores:
 *                                  ProfileFacts (= Loops + perfil).
 *      Tipo C (analysis/hw/)       describen el HARDWARE (Target): <-- ESTE
 *                                  MachineCostFacts (latency/tp/ports/uops).
 *
 * No depende del CFG.  No depende del IR.  Depende del TARGET.  Por eso el
 * flujo de decision tiene DOS fuentes ortogonales:
 *
 *      Programa                        Hardware
 *         |                               |
 *         v                               v
 *   Facts del programa   +   Facts del hardware (MachineCostFacts)
 *                        |
 *                        v
 *                    Decision
 *
 * TRAZABILIDAD (ANAMNESIS): al ser un Fact almacenado, la decision se vuelve
 * explicable recorriendo la cadena hacia atras:
 *
 *      Decision -> ObjectiveTerms -> MachineCostFacts -> SchedCostModel
 *
 * es decir, "por que elegiste este spill" se responde siguiendo los Facts que
 * lo sostienen (coste del reload/store de ESTA microarquitectura) en vez de
 * recalcular.
 *
 * INDEPENDENCIA DEL BACKEND (regla del usuario): este Fact contiene SOLO la
 * estructura abstracta @c InstrCost (jit/sched/instr_cost.h), nunca un @c
 * MInstr. El UNICO que entiende la ISA es el backend, que PRODUCE estos @c
 * InstrCost (probe_machine_cost_facts, en jit/) preguntando al @c
 * SchedCostModel con las operaciones sinteticas del allocator.  El
 * snapshot/allocator los CONSUME abstractos -> sigue siendo independiente del
 * backend.
 *
 *      SchedCostModel  --produce-->  InstrCost  --empaqueta--> MachineCostFacts
 *      (backend, ISA)                (abstracto)                (Fact Tipo C)
 */

#ifndef VESTA_ANALYSIS_HW_MACHINE_COST_FACTS_H
#define VESTA_ANALYSIS_HW_MACHINE_COST_FACTS_H

#include "jit/sched/instr_cost.h"

#include <vector>

namespace analysis {

/**
 * @struct MachineCostFacts
 * @brief Coste de las operaciones que le importan al asignador de recursos, en
 * la microarquitectura objetivo.  DATOS del hardware (Tipo C).
 *
 * Cada campo es un @c InstrCost abstracto (latencia + throughput + puertos), NO
 * una instruccion.  Los rellena el backend (unico que traduce a la ISA); los
 * consumidores ISA-neutrales (Objective via CostAdapter) los leen tal cual.
 */
struct MachineCostFacts {
    jit::sched::InstrCost
        load; ///< reload de un valor derramado (leer de la pila).
    jit::sched::InstrCost store; ///< spill de un valor a la pila (escribir).
    jit::sched::InstrCost
        move; ///< copia registro-registro (coalescing / two-address).
    bool from_uarch = false; ///< true = uarch exacta (--cpu); false = generico.
    const char *model_name = ""; ///< nombre del SchedCostModel (trazabilidad).
};

/**
 * @enum MachineCostCheck
 * @brief Comprobaciones de autocertificacion de @c MachineCostFacts (DATOS).
 */
enum class MachineCostCheck {
    LOAD_KIND_WRONG,  ///< load.kind != LOAD.
    STORE_KIND_WRONG, ///< store.kind != STORE.
    LATENCY_NONPOS,   ///< una latencia <= 0 (coste imposible).
    RECIP_TP_NONPOS,  ///< un throughput reciproco <= 0.
};

/// Hallazgo de la autocertificacion (DATO, no mensaje; i18n aguas arriba).
struct MachineCostIssue {
    MachineCostCheck check;
};

/**
 * @brief AUTOCERTIFICA los Facts de coste: kinds coherentes + latencias/tp > 0.
 *        Un coste <= 0 significaria "gratis o negativo", que romperia el
 *        Objective (una operacion nunca es gratis).
 */
inline std::vector<MachineCostIssue> validate(const MachineCostFacts &m) {
    std::vector<MachineCostIssue> out;
    if (m.load.kind != jit::sched::ExecKind::LOAD)
        out.push_back({MachineCostCheck::LOAD_KIND_WRONG});
    if (m.store.kind != jit::sched::ExecKind::STORE)
        out.push_back({MachineCostCheck::STORE_KIND_WRONG});
    for (const jit::sched::InstrCost *c : {&m.load, &m.store, &m.move}) {
        if (c->latency <= 0.0f)
            out.push_back({MachineCostCheck::LATENCY_NONPOS});
        if (c->recip_tp <= 0.0f)
            out.push_back({MachineCostCheck::RECIP_TP_NONPOS});
    }
    return out;
}

} // namespace analysis

#endif // VESTA_ANALYSIS_HW_MACHINE_COST_FACTS_H
