/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/adapters/cost_adapter.h
 * @brief CostAdapter: traduce los Facts de HARDWARE (MachineCostFacts) a los
 *        parametros de coste ISA-neutrales que consume el Objective.
 *
 * Es el mismo patron que los adaptadores del programa (Type/Liveness/Loop/
 * Profile/Const): TRADUCE informacion existente, no inventa logica.  La fuente
 * aqui es un Fact Tipo C (el hardware) en vez del IR:
 *
 *      MachineCostFacts (InstrCost)  --CostAdapter-->  SpillCostCard  --> Objective
 *          (Fact del hardware)                          (parametros ISA-neutrales)
 *
 * CLAVE (regla del usuario): el adaptador consume UNICAMENTE la estructura
 * abstracta @c InstrCost (dentro de @c MachineCostFacts), NUNCA un @c MInstr.  Asi
 * el backend sigue siendo el unico que entiende la ISA, y el Objective/allocator
 * permanecen independientes del backend.
 *
 * i18n: produce NUMEROS, no diagnosticos -> sin catalogo.
 * Fase 0.25: ADITIVO, funcion pura, sin consumidores (solo el test).
 */

#ifndef VESTA_CODEGEN_RBANK_ADAPTERS_COST_ADAPTER_H
#define VESTA_CODEGEN_RBANK_ADAPTERS_COST_ADAPTER_H

#include "analysis/hw/machine_cost_facts.h"
#include "codegen/rbank/objective.h"

namespace codegen {
namespace rbank {

/**
 * @brief Traduce los @c MachineCostFacts del hardware a la @c SpillCostCard que
 *        el Objective consume (reload/store/move en ciclos).  No toca @c MInstr.
 */
inline SpillCostCard spill_card_from(const analysis::MachineCostFacts &hw) noexcept {
    SpillCostCard c;
    c.reload_latency = static_cast<double>(hw.load.latency);
    c.store_latency  = static_cast<double>(hw.store.latency);
    c.move_latency   = static_cast<double>(hw.move.latency);
    c.from_hw        = true;
    return c;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_ADAPTERS_COST_ADAPTER_H
