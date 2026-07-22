/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/allocation_timeline.h
 * @brief @c AllocationTimeline: el modelo TEMPORAL de la asignacion.  Responde UNA
 *        pregunta FUNDAMENTAL -- ¿donde vive cada valor en cada instante?
 *
 * Es el formato que consume el Rewrite.  A diferencia de @c RegAlloc (una ubicacion
 * por vreg para toda su vida -- formato PLANO), aqui cada vreg tiene una linea
 * temporal de SEGMENTOS: en @c [from, to) esta en tal ubicacion, en el siguiente
 * tramo en otra.  RegAlloc = caso de 1 segmento por vreg; el Splitting produce varios.
 *
 * Por su pregunta fundamental, esta estructura tiende a ser el IR DE ASIGNACION del
 * backend: el punto donde convergen Recovery, Splitting, rematerializacion parcial y
 * futuras optimizaciones (todas producen un @c SplitPlan que el @c TimelineBuilder
 * materializa aqui; el Rewrite no sabe cual lo creo).
 *
 * Posiciones en el dominio MachineIR (@c codegen::LinearPos), semiabiertas [from, to)
 * como los @c LiveRange.  La @c Location de un segmento reusa @c RegAlloc::VAssign
 * (REG r / SPILL slot / NONE) -- no se duplica el vocabulario de "donde".
 */

#ifndef VESTA_CODEGEN_ALLOCATION_TIMELINE_H
#define VESTA_CODEGEN_ALLOCATION_TIMELINE_H

#include "codegen/linear_pos.h"
#include "codegen/regalloc.h" // RegAlloc::VAssign como vocabulario de "donde".

#include <cstdint>
#include <vector>

namespace codegen {

/// Ubicacion de un valor durante un tramo: reusa el vocabulario de RegAlloc
/// (REG fisico / SPILL slot / NONE), sin inventar uno nuevo.
using Location = RegAlloc::VAssign;

/**
 * @struct LocationSegment
 * @brief El valor vive en @c location durante @c [from, to) (dominio MachineIR).
 */
struct LocationSegment {
    LinearPos from;      ///< inicio del tramo (inclusive).
    LinearPos to;        ///< fin del tramo (exclusive).
    Location  location;  ///< donde vive el valor en [from, to).
};

/**
 * @struct ValueLocationTimeline
 * @brief Linea temporal de un vreg: sus segmentos ordenados por @c from y disjuntos.
 *        Un vreg muerto (sin usos) no tiene segmentos.
 */
struct ValueLocationTimeline {
    uint32_t vreg = 0;
    std::vector<LocationSegment> segments;

    /// Ubicacion en la posicion @p pos, o nullptr si ningun segmento la cubre (el
    /// valor no esta vivo ahi).  Busqueda lineal (pocos segmentos por vreg).
    const Location *at(LinearPos pos) const noexcept {
        for (const LocationSegment &s : segments)
            if (s.from <= pos && pos < s.to) return &s.location;
        return nullptr;
    }
};

/**
 * @struct AllocationTimeline
 * @brief El modelo temporal completo: la linea de cada vreg.  Indexable por vreg
 *        denso (los muertos quedan con @c segments vacio).
 */
struct AllocationTimeline {
    std::vector<ValueLocationTimeline> values; ///< por vreg id (denso 0..vreg_count-1).

    const ValueLocationTimeline *of(uint32_t vreg) const noexcept {
        return vreg < values.size() ? &values[vreg] : nullptr;
    }
};

} // namespace codegen

#endif // VESTA_CODEGEN_ALLOCATION_TIMELINE_H
