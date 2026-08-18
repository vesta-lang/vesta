/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/timeline_builder.h
 * @brief @c TimelineBuilder: la TRANSFORMACION que materializa un @c
 * AllocationTimeline a partir de una asignacion base (@c RegAlloc) + un plan de
 * modificaciones.
 *
 *     RegAlloc (asignacion plana) + <un>Plan (intencion) -> AllocationTimeline
 *
 * NO DECIDE NADA -- solo materializa (la decision es de quien produjo el plan;
 * por eso el plan trae la ubicacion DESTINO de cada tramo).  Y NO es "el
 * builder del splitting": es el MATERIALIZADOR del modelo temporal.  Hoy
 * consume un @c AssignmentPlan (de la Fragmentation Recovery); el dia que
 * existan un @c RematerializationPlan o un @c CoalescingPlan deberia
 * materializarlos igual, sin cambiar de naturaleza.  Esa es la tentacion
 * peligrosa a vigilar: que el builder empiece a DECIDIR que partir.  No.  Sigue
 * siendo un constructor.
 *
 * Es tambien el UNICO punto donde se traduce la representacion del allocator
 * (@c RegAlloc::VAssign) a la abstraccion del timeline (@c ValueLocation): una
 * vez, al construir -- nunca en cada consulta ni en el consumidor.
 */

#ifndef VESTA_CODEGEN_TIMELINE_BUILDER_H
#define VESTA_CODEGEN_TIMELINE_BUILDER_H

#include "codegen/allocation_result.h" // AllocationResult (FrameLayout + AllocationTimeline)
#include "codegen/regalloc.h"
#include "codegen/assignment_plan.h"
#include "jit/interval.h" // jit::IntervalResult / LiveInterval / LiveRange

#include <cstdint>
#include <vector>

namespace codegen {

/// Traduce la representacion del ALLOCATOR (@c RegAlloc::VAssign) a la
/// abstraccion del timeline.  Unico sitio donde se mira el enum interno de la
/// asignacion.
inline ValueLocation to_resolved(const RegAlloc::VAssign &va) noexcept {
    if (va.loc == RegAlloc::Loc::REG)
        return ValueLocation{ValueLocation::Register{va.reg}};
    if (va.loc == RegAlloc::Loc::SPILL)
        return ValueLocation{ValueLocation::Stack{va.slot}};
    return ValueLocation{};
}

/**
 * @brief Aplica UNA afirmacion del plan sobre los segmentos de un vreg: parte
 * los que solapan el tramo y pone dentro la ubicacion que el intervalo afirma.
 * Los segmentos quedan ordenados y disjuntos (el corte preserva el orden).
 *
 * No sabe -- ni le importa -- si el intervalo nacio de la Recovery, del
 * Splitting, de una rematerializacion o de un coalescing: solo transforma una
 * representacion temporal.  Por eso no se llama "apply_split".
 *
 *     base:  [----------- MEM -----------)
 *     split:        [-- REG --)
 *     out:   [ MEM )[-- REG --)[-- MEM --)
 */
inline void apply_interval(std::vector<LocationSegment> &segs,
                           const AssignmentInterval &si) {
    std::vector<LocationSegment> out;
    out.reserve(segs.size() + 2);
    for (const LocationSegment &s : segs) {
        if (s.to <= si.from || si.to <= s.from) { // sin solape: intacto.
            out.push_back(s);
            continue;
        }
        if (s.from < si.from) // tramo previo: sigue la base.
            out.push_back({s.from, si.from, s.location});
        // Interseccion: la ubicacion que pidio el plan.
        const LinearPos a = (s.from < si.from) ? si.from : s.from;
        const LinearPos b = (si.to < s.to) ? si.to : s.to;
        out.push_back({a, b, si.location});
        if (si.to < s.to) // tramo posterior: vuelve a la base.
            out.push_back({si.to, s.to, s.location});
    }
    segs.swap(out);
}

/**
 * @brief Materializa el @c AllocationTimeline desde @p ra (asignacion plana) +
 * @p ivs (los rangos vivos por vreg, opcional) + @p plan (modificaciones
 * temporales).
 * @return El modelo temporal, indexable por vreg denso.
 *
 * Sin @p ivs (caller que no construye intervals) cada vreg vivo recibe un unico
 * segmento que cubre toda la funcion: equivalente a la asignacion plana para un
 * consumidor que pregunta por posiciones reales.  Con @p plan vacio el
 * resultado es exactamente la
 * @c RegAlloc (1 segmento por rango): el pipeline SIEMPRE existe, sin casos
 * especiales.
 */
inline AllocationTimeline
build_allocation_timeline(const RegAlloc &ra, const jit::IntervalResult *ivs,
                          const AssignmentPlan &plan) {
    AllocationTimeline tl;

    /* (1) Segmentos BASE: la asignacion plana proyectada en el tiempo. */
    if (ivs) {
        // Con rangos: un segmento por @c LiveRange (respeta los huecos de
        // liveness). [from, to) semiabierto igual que el LiveRange -> sin
        // off-by-one.
        const uint32_t n = static_cast<uint32_t>(ivs->intervals.size());
        tl.values.resize(n);
        for (uint32_t vreg = 0; vreg < n; ++vreg) {
            const jit::LiveInterval &li = ivs->intervals[vreg];
            ValueLocationTimeline &vt = tl.values[vreg];
            vt.vreg = vreg;
            if (li.ranges.empty()) continue; // vreg muerto -> sin segmentos.
            const ValueLocation loc = (vreg < ra.assign.size())
                                          ? to_resolved(ra.assign[vreg])
                                          : ValueLocation{};
            vt.segments.reserve(li.ranges.size());
            for (const jit::LiveRange &r : li.ranges)
                vt.segments.push_back(
                    {LinearPos{r.from}, LinearPos{r.to}, loc});
        }
    } else {
        // Sin rangos: un unico segmento que cubre TODA la funcion [0, MAX) por
        // cada vreg con ubicacion real.  El fin usa el maximo (no @c invalid():
        // aqui MAX es un extremo real "hasta el final", no un estado).
        const uint32_t n = static_cast<uint32_t>(ra.assign.size());
        tl.values.resize(n);
        for (uint32_t vreg = 0; vreg < n; ++vreg) {
            ValueLocationTimeline &vt = tl.values[vreg];
            vt.vreg = vreg;
            const ValueLocation loc = to_resolved(ra.assign[vreg]);
            if (loc.is_none()) continue; // muerto -> sin segmentos.
            vt.segments.push_back({LinearPos{0}, LinearPos{0xFFFFFFFFu}, loc});
        }
    }

    /* (2) Aplicar el PLAN: cada tramo parte los segmentos de su vreg.  El
     * builder no decide nada -- solo coloca la ubicacion que el plan pidio. */
    for (const AssignmentInterval &si : plan.intervals) {
        if (si.vreg >= tl.values.size())
            continue; // vreg fuera de rango: ignorar.
        apply_interval(tl.values[si.vreg].segments, si);
    }
    return tl;
}

/**
 * @brief Ensambla el @c AllocationResult que consume el Rewrite: el @c
 * FrameLayout (derivado de la @c RegAlloc -- callee-saved + slots, no temporal)
 * + el
 *        @c AllocationTimeline.  Es el unico artefacto que el Rewrite ve; cada
 * mitad responde su propia pregunta.
 */
inline AllocationResult build_allocation_result(const RegAlloc &ra,
                                                const jit::IntervalResult *ivs,
                                                const AssignmentPlan &plan) {
    AllocationResult ar;
    ar.frame.callee_saved_used = ra.callee_saved_used;
    ar.frame.num_spill_slots = ra.num_spill_slots;
    ar.timeline = build_allocation_timeline(ra, ivs, plan);
    return ar;
}

} // namespace codegen

#endif // VESTA_CODEGEN_TIMELINE_BUILDER_H
