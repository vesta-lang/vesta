/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/timeline_builder.h
 * @brief @c TimelineBuilder: la TRANSFORMACION que materializa un @c AllocationTimeline
 *        a partir de una asignacion base (@c RegAlloc) + un plan de modificaciones.
 *
 *     RegAlloc (asignacion plana) + <un>Plan (intencion) -> AllocationTimeline
 *
 * NO DECIDE NADA -- solo materializa (la decision es de quien produjo el plan).  Y NO es
 * "el builder del splitting": es el MATERIALIZADOR del modelo temporal.  Hoy consume un
 * @c SplitPlan (de la Fragmentation Recovery); el dia que existan un @c RematerializationPlan
 * o un @c CoalescingPlan deberia materializarlos igual, sin cambiar de naturaleza.  Esa es
 * la tentacion peligrosa a vigilar cuando llegue el splitting: que el builder empiece a
 * DECIDIR que partir.  No.  Sigue siendo un constructor -- por eso es una pieza estable.
 *
 * ESTADO (incremento 1 del Splitting): implementa el caso TRIVIAL -- @c SplitPlan
 * vacio -> cada vreg vivo recibe UN segmento por cada @c LiveRange con su ubicacion
 * de @c RegAlloc.  Ese timeline es EQUIVALENTE a la RegAlloc (mismo resultado en toda
 * posicion viva); el Rewrite podra consumirlo sin cambiar el codigo emitido antes de
 * que exista un split real (diff_harness verde).  El manejo de @c SplitInterval (partir
 * un segmento en MEM/REG/MEM) es el SIGUIENTE incremento.
 */

#ifndef VESTA_CODEGEN_TIMELINE_BUILDER_H
#define VESTA_CODEGEN_TIMELINE_BUILDER_H

#include "codegen/allocation_result.h" // AllocationResult (FrameLayout + AllocationTimeline)
#include "codegen/regalloc.h"
#include "codegen/split_plan.h"
#include "jit/interval.h" // jit::IntervalResult / LiveInterval / LiveRange

#include <cassert>
#include <cstdint>

namespace codegen {

/**
 * @brief Materializa el @c AllocationTimeline desde @p ra (asignacion plana) + @p ivs
 *        (los rangos vivos por vreg) + @p plan (intenciones de split).
 * @return El modelo temporal, indexable por vreg denso.
 *
 * Incremento 1: solo el caso trivial (@p plan vacio).  Un @p plan con puntos aun no
 * se procesa -- se afirma en debug para cazar un uso prematuro; en release produce el
 * timeline plano (conservador: nunca emite algo incorrecto, solo ignora el split).
 */
inline AllocationTimeline build_allocation_timeline(const RegAlloc &ra,
                                                    const jit::IntervalResult *ivs,
                                                    const SplitPlan &plan) {
    assert(plan.empty() &&
           "build_allocation_timeline: manejo de SplitInterval aun no implementado "
           "(incremento 1 = caso trivial); el split real llega en el siguiente paso");
    (void)plan; // reservado para el siguiente incremento (procesar los SplitInterval).

    AllocationTimeline tl;
    if (ivs) {
        // Con rangos: un segmento por @c LiveRange (respeta los huecos de liveness).
        // [from, to) semiabierto igual que el LiveRange -> sin off-by-one.
        const uint32_t n = static_cast<uint32_t>(ivs->intervals.size());
        tl.values.resize(n);
        for (uint32_t vreg = 0; vreg < n; ++vreg) {
            const jit::LiveInterval &li = ivs->intervals[vreg];
            ValueLocationTimeline &vt = tl.values[vreg];
            vt.vreg = vreg;
            if (li.ranges.empty()) continue; // vreg muerto -> sin segmentos.
            const Location loc =
                (vreg < ra.assign.size()) ? ra.assign[vreg] : Location{};
            vt.segments.reserve(li.ranges.size());
            for (const jit::LiveRange &r : li.ranges)
                vt.segments.push_back({LinearPos{r.from}, LinearPos{r.to}, loc});
        }
    } else {
        // Sin rangos (caller que no construye intervals): un unico segmento que cubre
        // TODA la funcion [0, MAX) por cada vreg con ubicacion real.  El rewrite trivial
        // no usa la posicion -> equivalente a la RegAlloc plana.  El fin usa el maximo
        // (no @c invalid(): aqui MAX es un extremo real "hasta el final", no un estado).
        const uint32_t n = static_cast<uint32_t>(ra.assign.size());
        tl.values.resize(n);
        for (uint32_t vreg = 0; vreg < n; ++vreg) {
            ValueLocationTimeline &vt = tl.values[vreg];
            vt.vreg = vreg;
            const Location loc = ra.assign[vreg];
            if (loc.loc == RegAlloc::Loc::NONE) continue; // muerto -> sin segmentos.
            vt.segments.push_back({LinearPos{0}, LinearPos{0xFFFFFFFFu}, loc});
        }
    }
    return tl;
}

/**
 * @brief Ensambla el @c AllocationResult que consume el Rewrite: el @c FrameLayout
 *        (derivado de la @c RegAlloc -- callee-saved + slots, no temporal) + el
 *        @c AllocationTimeline (via @c build_allocation_timeline).  Es el unico
 *        artefacto que el Rewrite ve; cada mitad responde su propia pregunta.
 */
inline AllocationResult build_allocation_result(const RegAlloc &ra,
                                                const jit::IntervalResult *ivs,
                                                const SplitPlan &plan) {
    AllocationResult ar;
    ar.frame.callee_saved_used = ra.callee_saved_used;
    ar.frame.num_spill_slots = ra.num_spill_slots;
    ar.timeline = build_allocation_timeline(ra, ivs, plan);
    return ar;
}

} // namespace codegen

#endif // VESTA_CODEGEN_TIMELINE_BUILDER_H
