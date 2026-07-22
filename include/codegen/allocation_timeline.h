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
#include "codegen/regalloc.h"          // RegAlloc::VAssign (representacion interna)
#include "codegen/resolved_location.h" // ResolvedLocation (abstraccion al consumidor)

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
 * @brief El modelo temporal PURO: la linea de cada vreg.  Responde SOLO ¿donde vive
 *        este valor en este instante?  El frame (callee-saved, slots) NO vive aqui --
 *        no es temporal; es otra pregunta (@c FrameLayout).  Indexable por vreg denso
 *        (los muertos quedan con @c segments vacio).
 *
 * El consumidor (Rewrite) usa SOLO @c lookup(vreg, pos) -- nunca @c segments ni @c at
 * directamente.  Eso oculta la representacion: manana el lookup puede ser busqueda
 * lineal / binary search / tabla / cache sin que el Rewrite se entere.  Y como hoy
 * SIEMPRE existe un segmento que cubre toda la vida, el lookup ya funciona igual con 1
 * o con N segmentos -> cuando llegue el splitting, el Rewrite NO se toca (solo el
 * TimelineBuilder genera mas segmentos).
 */
struct AllocationTimeline {
    std::vector<ValueLocationTimeline> values; ///< por vreg id (denso 0..vreg_count-1).

    const ValueLocationTimeline *of(uint32_t vreg) const noexcept {
        return vreg < values.size() ? &values[vreg] : nullptr;
    }
    /// Ubicacion de @p vreg en @p pos como ABSTRACCION (@c ResolvedLocation).  UNICA via
    /// del consumidor -- no expone @c segments (independencia de la representacion
    /// temporal) NI @c Location (independencia de como se representa una ubicacion): el
    /// Rewrite pregunta is_register()/is_stack(), nunca compara un enum.
    ResolvedLocation lookup(uint32_t vreg, LinearPos pos) const noexcept {
        const ValueLocationTimeline *t = of(vreg);
        return to_resolved(t ? t->at(pos) : nullptr);
    }
    /// Ubicacion del PRIMER tramo de @p vreg (o @c none() si muerto).  EXCEPCION para
    /// consultas SIN posicion de valores que NO se fragmentan -- register-bound de inline
    /// asm: una unica ubicacion en toda su vida, sin ambiguedad de momento.  NO usar en el
    /// hot path por-operando (ahi el momento SIEMPRE se conoce: use/def).
    ResolvedLocation first_location(uint32_t vreg) const noexcept {
        const ValueLocationTimeline *t = of(vreg);
        return to_resolved((t && !t->segments.empty())
                               ? &t->segments.front().location
                               : nullptr);
    }

  private:
    /// Traduce la representacion interna (@c Location = REG/SPILL/NONE) a la abstraccion
    /// @c ResolvedLocation.  La comparacion del enum vive AQUI (en el materializador del
    /// modelo temporal), NUNCA en el consumidor.
    static ResolvedLocation to_resolved(const Location *l) noexcept {
        if (!l) return ResolvedLocation{};
        if (l->loc == RegAlloc::Loc::REG)
            return ResolvedLocation{ResolvedLocation::Register{l->reg}};
        if (l->loc == RegAlloc::Loc::SPILL)
            return ResolvedLocation{ResolvedLocation::Stack{l->slot}};
        return ResolvedLocation{};
    }
};

} // namespace codegen

#endif // VESTA_CODEGEN_ALLOCATION_TIMELINE_H
