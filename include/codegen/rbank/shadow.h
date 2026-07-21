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

/**
 * @brief Mapea la clase del backend (jit::RegClass) a la del modelo.
 *
 * HERENCIA HISTORICA (a vigilar, no cambiar ahora): @c RegClass::FP se mapea a
 * @c FP_VECTOR porque el backend SSE usa XMM para TODO (escalar y vectorial), asi
 * que hoy coinciden FISICAMENTE.  Pero conceptualmente FP-escalar != FP-vectorial;
 * el dia que el banco los distinga (o un target los separe) esto debera devolver un
 * @c ResourceClass::FP escalar propio aunque comparta banco.  Es un sitio donde se
 * nota que el modelo aun hereda la union XMM del backend.
 */
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
    uint32_t allocated_gp    = 0; ///< valores GP en registro (no spilled).
    uint32_t allocated_fp    = 0; ///< valores FP en registro (no spilled).
    uint32_t pinned_values   = 0; ///< valores con pin (fixed_reg >= 0).
    double   spill_cost      = 0.0; ///< coste total de spill (via Objective).
};

/**
 * @struct ShadowDiff
 * @brief Diferencia rbank - linear (negativo = rbank MEJOR).  Hace interpretable el
 *        shadow sobre miles de funciones: "-3 spills, -4.5 coste, +2 copies".
 */
struct ShadowDiff {
    int    spills_delta       = 0;
    double spill_cost_delta   = 0.0;
    int    copies_delta       = 0; ///< copies_removed (rbank suele > linear).
    int    pressure_gp_delta  = 0;
    int    pressure_fp_delta  = 0;
    int    allocated_gp_delta = 0;
    int    allocated_fp_delta = 0;
};

/** @brief Diff rbank - linear (negativo en spills/coste = rbank mejora). */
inline ShadowDiff shadow_diff(const ShadowStats &lin, const ShadowStats &rb) {
    ShadowDiff d;
    d.spills_delta       = static_cast<int>(rb.spills) - static_cast<int>(lin.spills);
    d.spill_cost_delta   = rb.spill_cost - lin.spill_cost;
    d.copies_delta       = static_cast<int>(rb.copies_removed) - static_cast<int>(lin.copies_removed);
    d.pressure_gp_delta  = static_cast<int>(rb.max_pressure_gp) - static_cast<int>(lin.max_pressure_gp);
    d.pressure_fp_delta  = static_cast<int>(rb.max_pressure_fp) - static_cast<int>(lin.max_pressure_fp);
    d.allocated_gp_delta = static_cast<int>(rb.allocated_gp) - static_cast<int>(lin.allocated_gp);
    d.allocated_fp_delta = static_cast<int>(rb.allocated_fp) - static_cast<int>(lin.allocated_fp);
    return d;
}

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
        if (v.req.fixed_reg >= 0) ++s.pinned_values;
        const bool is_gp = v.req.cls == ResourceClass::GP;
        if (v.value_id < ra.assign.size() &&
            ra.assign[v.value_id].loc == jit::RegAlloc::Loc::SPILL) {
            ++s.spills;
            s.spill_cost += spill_cost_via_objective(v.req, ctx);
        } else if (v.value_id < ra.assign.size() &&
                   ra.assign[v.value_id].loc == jit::RegAlloc::Loc::REG) {
            if (is_gp) ++s.allocated_gp; else ++s.allocated_fp;
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
    // allocated cuenta LANES FISICAS ASIGNABLES.  Hoy "lane != spilled" <=> registro
    // fisico (el unico recurso no-memoria del banco), pero el dia que existan scratch
    // / stack-cache / pseudo-lanes / memoria persistente como recursos, "no spilled"
    // dejara de implicar "en registro" -> habra que distinguir por tipo de lane.
    for (const AbstractValue &v : co.problem.values) {
        if (v.req.fixed_reg >= 0) ++s.pinned_values;
        if (la.lane_of(v.value_id) == kSpilled) continue;
        if (v.req.cls == ResourceClass::GP) ++s.allocated_gp; else ++s.allocated_fp;
    }
    return s;
}

/**
 * @struct ShadowReport
 * @brief Veredicto de UNA funcion: las dos stats + el diff + si rbank es equivalente
 *        o mejora.  El agregador (ShadowAggregate) los suma sobre el corpus.
 *
 * Criterio (calidad del allocator, no bit-igualdad del codigo): la metrica que
 * importa es spills y su coste.  @c improved = rbank mejora en spills, o iguala
 * spills con menos coste, SIN empeorar.  @c equivalent = ni mejora ni empeora.
 * "Empeora" = ni equivalent ni improved.
 */
struct ShadowReport {
    ShadowStats linear;
    ShadowStats rbank;
    ShadowDiff  diff;
    bool        equivalent = false;
    bool        improved   = false;
};

/** @brief Compara dos stats -> ShadowReport (diff + veredicto equivalente/mejora). */
inline ShadowReport make_shadow_report(const ShadowStats &lin, const ShadowStats &rb) {
    ShadowReport r;
    r.linear = lin;
    r.rbank = rb;
    r.diff = shadow_diff(lin, rb);
    const double EPS = 1e-6;
    const bool worse = rb.spills > lin.spills ||
                       (rb.spills == lin.spills && rb.spill_cost > lin.spill_cost + EPS);
    const bool better = rb.spills < lin.spills ||
                        (rb.spills == lin.spills && rb.spill_cost < lin.spill_cost - EPS);
    r.improved = better && !worse;
    r.equivalent = !better && !worse;
    return r;
}

/**
 * @struct ShadowAggregate
 * @brief Panel de calidad del allocator sobre TODO un corpus: cuantas funciones
 *        igualan / mejoran / empeoran + totales de spills y coste.  Es la salida que
 *        el paso 2 (shadow en vreg_pipeline sobre cada funcion) acumula.
 */
struct ShadowAggregate {
    uint32_t functions        = 0;
    uint32_t equal            = 0;
    uint32_t improved         = 0;
    uint32_t worsened         = 0;
    uint64_t linear_spills    = 0;
    uint64_t rbank_spills     = 0;
    double   linear_spill_cost = 0.0;
    double   rbank_spill_cost  = 0.0;

    /** @brief Acumula el veredicto de una funcion. */
    void add(const ShadowReport &r) {
        ++functions;
        if (r.improved) ++improved;
        else if (r.equivalent) ++equal;
        else ++worsened;
        linear_spills += r.linear.spills;
        rbank_spills += r.rbank.spills;
        linear_spill_cost += r.linear.spill_cost;
        rbank_spill_cost += r.rbank.spill_cost;
    }
};

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_SHADOW_H
