/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/adapters/type_adapter.h
 * @brief Adaptador de TIPO (Fase 0.25): @c ir::IrType -> clase de recurso +
 *        ancho de vista en un @c ValueRequirements.
 *
 * DISCIPLINA DE LOS ADAPTADORES (Fase 0.25): un adaptador SOLO TRADUCE
 * informacion que YA existe; NO inventa logica.  El @c TypeAdapter mira el tipo
 * del valor y rellena EXCLUSIVAMENTE @c cls y @c width.  NO decide si cruza
 * llamadas, si es rematerializable, si conviene derramarlo -- esos hechos
 * vienen de OTROS adaptadores (Liveness, Const, Loop, Alias, Profile), cada uno
 * en su fichero, pequeno y testeable.  Asi, cuando algo salga mal, la pregunta
 * "por que este valor tiene X?" tiene una respuesta de UN adaptador, no de un
 * bloque de 500 lineas.
 *
 * SNAPSHOT: los adaptadores rellenan UN mismo @c ValueRequirements durante su
 * construccion; despues es INMUTABLE.  El resto del modelo (Constraints,
 * Objective, DecisionEngine) trabaja siempre sobre ese estado congelado, nunca
 * consultando el IR de nuevo.
 *
 * CENTRALIZACION: el ancho se toma de @c analysis::memory_access_size (la
 * fuente de la capa de Facts), NO de una tabla propia -> una sola verdad
 * IrType->bytes.
 *
 * Fase 0.25: ADITIVO, sin cambio de comportamiento (nadie lo consume aun salvo
 * el test; el wiring al codegen es de fases posteriores).
 */

#ifndef VESTA_CODEGEN_RBANK_ADAPTERS_TYPE_ADAPTER_H
#define VESTA_CODEGEN_RBANK_ADAPTERS_TYPE_ADAPTER_H

#include "analysis/memory/memory_access.h"
#include "ir/ssa_ir.h"
#include "codegen/rbank/value_requirements.h"

namespace codegen {
namespace rbank {

/** @brief True si el tipo produce un valor asignable (VOID no). */
inline bool type_has_value(ir::IrType t) noexcept {
    return t != ir::IrType::VOID;
}

/**
 * @brief Clase de recurso de un @c IrType (traduccion pura, sin heuristica).
 *
 * F32/F64 -> banco float/vector; todo lo demas (enteros con/sin signo, puntero
 * host, GcHandle, bool) -> banco entero de proposito general.
 *
 * Se escribe como @c switch aunque hoy haya dos casos: es el PUNTO DE EXTENSION
 * natural para cuando entren tipos vector<N> / SIMD (-> FP_VECTOR), mascara
 * (-> MASK) o predicado (-> PREDICATE).  El sitio ya esta preparado.
 */
inline ResourceClass type_adapter_class(ir::IrType t) noexcept {
    switch (t) {
    case ir::IrType::F32:
    case ir::IrType::F64: return ResourceClass::FP_VECTOR;
    // Futuro: vector<N>/SIMD -> FP_VECTOR; mascara -> MASK; predicado ->
    // PREDICATE.
    default: return ResourceClass::GP;
    }
}

/**
 * @brief Ancho de vista de un @c IrType, tomado de la fuente canonica de la
 *        capa de Facts (@c analysis::memory_access_size): 1/2/4/8 bytes.
 */
inline ViewWidth type_adapter_width(ir::IrType t) noexcept {
    return view_width_for_bytes(
        static_cast<uint32_t>(analysis::memory_access_size(t)));
}

/**
 * @brief Rellena EXCLUSIVAMENTE @c cls y @c width de @p r desde el tipo @p t.
 *
 * No toca ningun otro campo (residency, crosses_call, is_gc, loop_depth,
 * rematerializable, fixed_reg) -- eso es de otros adaptadores.  El nombre
 * @c populate_* es el verbo comun de todos los adaptadores de Fase 0.25.
 */
inline void populate_type_requirements(ValueRequirements &r,
                                       ir::IrType t) noexcept {
    r.cls = type_adapter_class(t);
    r.width = type_adapter_width(t);
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_ADAPTERS_TYPE_ADAPTER_H
