/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/rbank/adapters/loop_adapter.h
 * @brief Adaptador de BUCLES (Fase 0.25): @c analysis::LoopFacts ->
 *        @c loop_depth en un @c ValueRequirements.
 *
 * Ejemplo de que un buen productor de Facts deja el adaptador RIDICULAMENTE
 * simple: toda la logica (deteccion de bucles, dominadores, profundidad) vive en
 * @c compute_loop_facts (analysis/facts/loop_facts.h); aqui SOLO se copia la
 * profundidad del bloque donde se define el valor.  Trazabilidad total:
 * "¿por que loop_depth=2?" -> "el LoopAdapter: el bloque de definicion tiene
 * profundidad 2 segun LoopFacts".
 *
 * Fase 0.25: ADITIVO, sin cambio de comportamiento.
 */

#ifndef VESTA_JIT_RBANK_ADAPTERS_LOOP_ADAPTER_H
#define VESTA_JIT_RBANK_ADAPTERS_LOOP_ADAPTER_H

#include "analysis/facts/loop_facts.h"
#include "ir/ssa_ir.h"
#include "jit/rbank/value_requirements.h"

namespace jit {
namespace rbank {

/**
 * @brief Rellena EXCLUSIVAMENTE @c loop_depth de @p r con la profundidad del
 *        bloque @p def_block (donde se define el valor) segun @p f.
 */
inline void populate_loop_requirements(ValueRequirements &r,
                                       const analysis::LoopFacts &f,
                                       ir::IrBlockId def_block) noexcept {
    r.loop_depth = static_cast<uint16_t>(f.depth_of(def_block));
}

} // namespace rbank
} // namespace jit

#endif // VESTA_JIT_RBANK_ADAPTERS_LOOP_ADAPTER_H
