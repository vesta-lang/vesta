/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/shadow.h
 * @brief SHADOW MODE (paso 1): adaptador IntervalResult -> AbstractProblem + metricas
 *        comparables, para correr rbank EN PARALELO a linear_scan sin cambiar el
 *        comportamiento y demostrar que el modelo decide IGUAL o MEJOR.
 *
 *     IntervalResult (MachineIR real)  --intervals_to_problem-->  AbstractProblem
 *              |                             (EXTRACT FACTS, nada mas)      |
 *         linear_scan -> RegAlloc                                  coalesce + smart_spill
 *              |                                                           |
 *         ShadowStats (linear)   <---- comparar ---->   ShadowStats (rbank)
 *
 * ADAPTADOR FINO (regla del usuario): @c intervals_to_problem solo EXTRAE los Facts
 * del backend (envolvente del LiveRange, clase, pin, cross-call, GC); NUNCA "arregla"
 * el problema.  Si empezara a decidir, la logica volveria a repartirse y el modelo
 * dejaria de ser LA representacion del allocator.  El adaptador es el punto donde se
 * decide QUE es del modelo y QUE sigue siendo detalle del backend -- y debe ser
 * minimo.
 *
 * PRIMERA VALIDACION EXTERNA: hasta ahora todo validaba propiedades INTERNAS
 * (coloreo propio, cromatico, cache, coalescing, spill).  El shadow es la primera
 * pregunta EXTERNA: "¿el modelo describe correctamente PROGRAMAS REALES?".
 *
 * LIMITACION CONOCIDA (envolvente): @c LiveInterval es MULTI-RANGO (@c ranges), pero
 * @c AbstractValue usa un solo [start,end] -> se toma el ENVOLVENTE [first.from,
 * last.to).  Pierde los huecos (misma Opcion B/RangeSet diferida que el coalescing).
 * @c LiveInterval.uses (las posiciones de uso = el NEXT-USE que el Belady real
 * necesita) NO se extrae aun -> entra con UseDefFacts.
 *
 * i18n: produce DATOS/numeros.  ADITIVO: shadow no cambia el codigo emitido.
 */

#ifndef VESTA_CODEGEN_RBANK_SHADOW_H
#define VESTA_CODEGEN_RBANK_SHADOW_H

#include "codegen/rbank/abstract_problem.h"
#include "codegen/rbank/coalesce.h"
#include "codegen/rbank/coloring.h"
#include "codegen/rbank/optimization_context.h"
#include "codegen/rbank/smart_spill.h"
#include "jit/interval.h"
#include "jit/linear_scan.h"

#include <cstdint>

namespace codegen {
namespace rbank {

/** @brief Mapea la clase del backend (jit::RegClass) a la del modelo. */
inline ResourceClass resource_class_from_reg(jit::RegClass c) {
    return c == jit::RegClass::FP ? ResourceClass::FP_VECTOR : ResourceClass::GP;
}

/**
 * @brief ADAPTADOR FINO: extrae un @c AbstractProblem de los intervalos reales.
 *        Solo EXTRAE Facts; no decide nada.
 *
 * Por cada @c LiveInterval no vacio: value_id=vreg, [start,end]=envolvente de los
 * ranges (semi-abierto [from,to) -> cerrado), clase, pin (fixed_reg), is_gc, y
 * crosses_call si algun rango cubre una call_position.  El copy-graph (afinidad)
 * se deja VACIO -- vendra del ssa_coalesce cuando se cablee.
 */
inline AbstractProblem intervals_to_problem(const jit::IntervalResult &ivs) {
    AbstractProblem p;
    p.values.reserve(ivs.intervals.size());
    for (const jit::LiveInterval &iv : ivs.intervals) {
        if (iv.ranges.empty()) continue; // vreg muerto: el allocator lo ignora.
        AbstractValue av;
        av.value_id = iv.vreg;
        av.start = iv.ranges.front().from;
        av.end = iv.ranges.back().to > 0 ? iv.ranges.back().to - 1 : 0; // ) -> ]
        av.req.value_id = iv.vreg;
        av.req.cls = resource_class_from_reg(iv.cls);
        av.req.width = iv.cls == jit::RegClass::FP ? ViewWidth::W16 : ViewWidth::W8;
        av.req.fixed_reg = static_cast<int16_t>(iv.fixed_reg); // -1 o el pin.
        av.req.is_gc = iv.gc_kind != 0;
        for (uint32_t cp : ivs.call_positions) {
            for (const jit::LiveRange &r : iv.ranges)
                if (cp >= r.from && cp < r.to) { av.req.crosses_call = true; break; }
            if (av.req.crosses_call) break;
        }
        p.values.push_back(av);
    }
    return p;
}

/**
 * @struct ShadowStats
 * @brief Metricas comparables de una asignacion (para linear_scan vs rbank).  El
 *        objetivo del shadow NO es solo igualdad, sino ver si el modelo decide MEJOR.
 */
struct ShadowStats {
    uint32_t values          = 0; ///< valores considerados (intervals no vacios).
    uint32_t spills          = 0; ///< valores derramados a memoria.
    uint32_t copies_removed  = 0; ///< afinidades realizadas (coalescing; 0 en linear).
    uint32_t max_pressure_gp = 0; ///< pico de solapamiento GP.
    uint32_t max_pressure_fp = 0; ///< pico de solapamiento FP.
    double   spill_cost      = 0.0; ///< coste total de spill (via Objective).
};

/** @brief Presion maxima (max_overlap) por clase de un problema. */
inline void fill_pressure(const AbstractProblem &p, ShadowStats &s) {
    s.max_pressure_gp = max_overlap(p, ResourceClass::GP);
    s.max_pressure_fp = max_overlap(p, ResourceClass::FP_VECTOR);
}

/**
 * @brief Metricas del linear_scan de PRODUCCION (RegAlloc) sobre el mismo problema.
 *        La presion se mide del problema (estructura); los spills, del RegAlloc.
 */
inline ShadowStats shadow_stats_linear(const jit::RegAlloc &ra,
                                       const AbstractProblem &p,
                                       const OptimizationContext &ctx) {
    ShadowStats s;
    s.values = static_cast<uint32_t>(p.values.size());
    fill_pressure(p, s);
    for (const AbstractValue &v : p.values) {
        if (v.value_id < ra.assign.size() &&
            ra.assign[v.value_id].loc == jit::RegAlloc::Loc::SPILL) {
            ++s.spills;
            s.spill_cost += spill_cost_via_objective(v.req, ctx);
        }
    }
    // linear_scan no hace coalescing por si mismo (lo hace ssa_coalesce antes) -> 0.
    return s;
}

/**
 * @brief Metricas de rbank: coalesce (F3) + smart_spill (F5) sobre el mismo problema.
 *        Es el pipeline del modelo end-to-end.
 */
inline ShadowStats shadow_stats_rbank(const AbstractProblem &p,
                                      const OptimizationContext &ctx,
                                      bool vec_active) {
    ShadowStats s;
    fill_pressure(p, s);
    const CoalesceResult co = coalesce_conservative(p);
    s.copies_removed = co.copies_eliminated;
    const LaneAssignment la = color_smart_spill(co.problem, ctx, vec_active);
    s.values = static_cast<uint32_t>(co.problem.values.size());
    s.spills = spill_count(co.problem, la);
    s.spill_cost = total_spill_cost(co.problem, la, ctx);
    return s;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_SHADOW_H
