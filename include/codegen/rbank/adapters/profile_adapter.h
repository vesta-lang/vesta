/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/adapters/profile_adapter.h
 * @brief Adaptador de PERFIL (Fase 0.25): @c analysis::ProfileFacts ->
 *        @c execution_weight en un @c ValueRequirements.
 *
 * Con el profiler CENTRALIZADO en @c ProfileFacts (fact Tipo B, derivado), el
 * adaptador es trivial: copia el peso de ejecucion del bloque de definicion.
 * Sin perfil, @c weight_of devuelve 0 y el @c OptimizationContext cae al
 * estimador estatico por @c loop_depth (LoopFacts) -> comportamiento identico.
 *
 * Fase 0.25: ADITIVO.
 */

#ifndef VESTA_CODEGEN_RBANK_ADAPTERS_PROFILE_ADAPTER_H
#define VESTA_CODEGEN_RBANK_ADAPTERS_PROFILE_ADAPTER_H

#include "analysis/derived/profile_facts.h"
#include "ir/ssa_ir.h"
#include "codegen/rbank/value_requirements.h"

namespace jit {
namespace rbank {

/**
 * @brief Rellena EXCLUSIVAMENTE @c execution_weight de @p r con el peso medido
 *        del bloque @p def_block segun @p pf (0 si no hay perfil).
 */
inline void populate_profile_requirements(ValueRequirements &r,
                                          const analysis::ProfileFacts &pf,
                                          ir::IrBlockId def_block) noexcept {
    r.execution_weight = pf.weight_of(def_block);
}

} // namespace rbank
} // namespace jit

#endif // VESTA_CODEGEN_RBANK_ADAPTERS_PROFILE_ADAPTER_H
