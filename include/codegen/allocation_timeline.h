/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/allocation_timeline.h
 * @brief @c AllocationTimeline: el modelo TEMPORAL de la asignacion.  Responde
 * UNA pregunta FUNDAMENTAL -- ¿donde vive cada valor en cada instante?
 *
 * Es el formato que consume el Rewrite.  A diferencia de @c RegAlloc (una
 * ubicacion por vreg para toda su vida -- formato PLANO), aqui cada vreg tiene
 * una linea temporal de SEGMENTOS: en @c [from, to) esta en tal ubicacion, en
 * el siguiente tramo en otra. RegAlloc = caso de 1 segmento por vreg; el
 * Splitting produce varios.
 *
 * Por su pregunta fundamental, esta estructura tiende a ser el IR DE ASIGNACION
 * del backend: el punto donde convergen Recovery, Splitting, rematerializacion
 * parcial y futuras optimizaciones (todas producen un plan que el @c
 * TimelineBuilder materializa aqui; el Rewrite no sabe cual lo creo).
 *
 * Habla SIEMPRE en la abstraccion @c ValueLocation -- nunca en la
 * representacion del allocator (@c RegAlloc::VAssign).  La traduccion ocurre
 * UNA vez, al construir (en el
 * @c TimelineBuilder), no en cada consulta.  El frame (callee-saved, slots) NO
 * vive aqui: no es temporal, es otra pregunta (@c FrameLayout).
 *
 * Posiciones en el dominio MachineIR (@c codegen::LinearPos), semiabiertas
 * [from, to) como los @c LiveRange.
 */

#ifndef VESTA_CODEGEN_ALLOCATION_TIMELINE_H
#define VESTA_CODEGEN_ALLOCATION_TIMELINE_H

#include "codegen/linear_pos.h"
#include "codegen/value_location.h"

#include <cstdint>
#include <vector>

namespace codegen {

/**
 * @struct LocationSegment
 * @brief El valor vive en @c location durante @c [from, to) (dominio
 * MachineIR).
 */
struct LocationSegment {
    LinearPos from;         ///< inicio del tramo (inclusive).
    LinearPos to;           ///< fin del tramo (exclusive).
    ValueLocation location; ///< donde vive el valor en [from, to).
};

/**
 * @struct ValueLocationTimeline
 * @brief Linea temporal de un vreg: sus segmentos ordenados por @c from y
 * disjuntos. Un vreg muerto (sin usos) no tiene segmentos.
 */
struct ValueLocationTimeline {
    uint32_t vreg = 0;
    std::vector<LocationSegment> segments;

    /// Ubicacion en @p pos, o @c none() si ningun segmento la cubre (no vive
    /// ahi). Busqueda lineal: pocos segmentos por vreg (1 sin split, 2-3 con
    /// split).
    ValueLocation at(LinearPos pos) const noexcept {
        for (const LocationSegment &s : segments)
            if (s.from <= pos && pos < s.to) return s.location;
        return ValueLocation{};
    }
};

/**
 * @struct AllocationTimeline
 * @brief El modelo temporal PURO: la linea de cada vreg.  Indexable por vreg
 * denso.
 *
 * El consumidor (Rewrite) usa SOLO @c lookup(vreg, pos) -- nunca @c segments.
 * Eso oculta la representacion: manana el lookup puede ser busqueda lineal /
 * binary search / tabla / cache sin que el Rewrite se entere.  Y como el lookup
 * funciona igual con 1 o con N segmentos, cuando llega el splitting el Rewrite
 * NO se toca: solo el TimelineBuilder genera mas segmentos.
 */
struct AllocationTimeline {
    std::vector<ValueLocationTimeline>
        values; ///< por vreg id (denso 0..vreg_count-1).

    const ValueLocationTimeline *of(uint32_t vreg) const noexcept {
        return vreg < values.size() ? &values[vreg] : nullptr;
    }
    /// Ubicacion de @p vreg en @p pos.  UNICA via del consumidor.
    ValueLocation lookup(uint32_t vreg, LinearPos pos) const noexcept {
        const ValueLocationTimeline *t = of(vreg);
        return t ? t->at(pos) : ValueLocation{};
    }
    /// Ubicacion del PRIMER tramo de @p vreg (o @c none() si muerto). EXCEPCION
    /// para consultas SIN posicion de valores que NO se fragmentan --
    /// register-bound de inline asm: una unica ubicacion en toda su vida, sin
    /// ambiguedad de momento.  NO usar en el hot path por-operando (ahi el
    /// momento SIEMPRE se conoce: use/def).
    ValueLocation first_location(uint32_t vreg) const noexcept {
        const ValueLocationTimeline *t = of(vreg);
        return (t && !t->segments.empty()) ? t->segments.front().location
                                           : ValueLocation{};
    }
};

} // namespace codegen

#endif // VESTA_CODEGEN_ALLOCATION_TIMELINE_H
