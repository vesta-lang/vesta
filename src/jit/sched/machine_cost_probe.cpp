/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file src/jit/sched/machine_cost_probe.cpp
 * @brief Implementacion del productor de MachineCostFacts (ver header).
 */

#include "jit/sched/machine_cost_probe.h"

#include "jit/machine_ir.h"
#include "jit/sched/cost_model.h"

#include <string>

namespace jit {
namespace sched {

analysis::MachineCostFacts probe_machine_cost_facts(const SchedCostModel &cm) {
    analysis::MachineCostFacts f;

    // Operando GP sintetico (RAX, qword): sirve de dst/addr/val de las probes.
    // El coste del reload/spill/move NO depende del registro concreto -- solo de
    // la familia de la operacion + ancho, que es lo que el cost model clasifica.
    const MOperand r = MOperand::make_reg(MReg::RAX, /*w=*/8);

    // Reload de un valor derramado:  dst = [addr]
    const MInstr ld = MInstr::make_load(r, r, /*width=*/8, /*sgn=*/false);
    // Spill de un valor a la pila:    [addr] = val
    const MInstr st = MInstr::make_store(r, r, /*width=*/8);
    // Copia registro-registro (coalescing / two-address roto).
    const MInstr mv = MInstr::make_unary(MOp::MOV, r, r);

    f.load  = cm.cost(ld);
    f.store = cm.cost(st);
    f.move  = cm.cost(mv);

    // Trazabilidad (ANAMNESIS): que modelo respondio.  El generico se llama
    // "generic"; cualquier otro nombre proviene de una uarch concreta (--cpu).
    f.model_name = cm.name();
    f.from_uarch = std::string(cm.name()) != "generic";
    return f;
}

} // namespace sched
} // namespace jit
