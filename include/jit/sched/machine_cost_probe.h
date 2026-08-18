/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/sched/machine_cost_probe.h
 * @brief Productor de MachineCostFacts (Tipo C) desde el backend.
 *
 * El backend es el UNICO que entiende la ISA: aqui construye las operaciones
 * sinteticas que le importan al asignador de recursos (reload/spill/move),
 * pregunta su coste al @c SchedCostModel y empaqueta los @c InstrCost
 * abstractos en un @c MachineCostFacts.  Ni el snapshot ni el allocator ven un
 * @c MInstr.
 *
 *      SchedCostModel  --cost(MInstr LOAD/STORE/MOV)-->  InstrCost
 *                                                          |
 *                                             empaqueta -> MachineCostFacts
 * (Tipo C)
 *
 * "El scheduler ya no calcula latencias: PUBLICA conocimiento" -- el mismo
 * movimiento que LoopFacts/ProfileFacts/ValueRequirements, ahora sobre el HW.
 */

#ifndef VESTA_JIT_SCHED_MACHINE_COST_PROBE_H
#define VESTA_JIT_SCHED_MACHINE_COST_PROBE_H

#include "analysis/hw/machine_cost_facts.h"

namespace jit {
namespace sched {

class SchedCostModel; // fwd (definido en cost_model.h).

/**
 * @brief Extrae los @c MachineCostFacts (coste de reload/spill/move) de @p cm.
 *        El backend construye los @c MInstr sinteticos; el Fact devuelto es
 *        abstracto (solo @c InstrCost).
 */
analysis::MachineCostFacts probe_machine_cost_facts(const SchedCostModel &cm);

} // namespace sched
} // namespace jit

#endif // VESTA_JIT_SCHED_MACHINE_COST_PROBE_H
