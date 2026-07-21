/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/adapters/const_adapter.h
 * @brief Adaptador de CONSTANTES (Fase 0.25): @c ir::IrValue.is_const ->
 *        @c rematerializable en un @c ValueRequirements.
 *
 * DISCIPLINA: solo TRADUCE informacion existente.  Un valor marcado como
 * constante literal (@c is_const) es REMATERIALIZABLE: en vez de derramarlo a
 * memoria y recargarlo, se puede RE-EMITIR la constante en cada uso (mas barato
 * que un load).  El @c Objective ya lo usa (@c spill_cost_of abarata el spill de
 * los rematerializables).
 *
 * ALCANCE (documentado, no heuristica): hoy se traduce EXCLUSIVAMENTE el flag
 * @c is_const (literales) -- la informacion que ya existe.  Otras formas
 * rematerializables (direcciones @c lea, @c STR_LIT_ADDR, ops de solo-constantes)
 * son un subconjunto MAYOR que se anadira cuando exista un analisis que las
 * marque; este adaptador es conservador (un falso-negativo solo pierde una
 * optimizacion, nunca corrompe).
 *
 * Fase 0.25: ADITIVO, sin cambio de comportamiento (solo el test lo consume).
 */

#ifndef VESTA_CODEGEN_RBANK_ADAPTERS_CONST_ADAPTER_H
#define VESTA_CODEGEN_RBANK_ADAPTERS_CONST_ADAPTER_H

#include "ir/ssa_ir.h"
#include "codegen/rbank/value_requirements.h"

namespace jit {
namespace rbank {

/**
 * @brief Rellena EXCLUSIVAMENTE @c rematerializable de @p r desde @p v.
 *
 * @c rematerializable = @c v.is_const (literal recomputable).  No toca ningun
 * otro campo.
 */
inline void populate_const_requirements(ValueRequirements &r,
                                        const ir::IrValue &v) noexcept {
    r.rematerializable = v.is_const;
}

} // namespace rbank
} // namespace jit

#endif // VESTA_CODEGEN_RBANK_ADAPTERS_CONST_ADAPTER_H
