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
        // TODO(RangeSet): el modelo PIERDE LOS HUECOS aqui -- toma el envolvente
        // [first.from, last.to) porque AbstractValue es de un solo segmento.  Casi
        // todos los problemas futuros CONVERGEN en este punto: el coalescing (check
        // de max_overlap por el envolvente), el next-use y el Belady real (necesitan
        // los usos intermedios) y la presion exacta.  Todos desaparecen cuando
        // AbstractValue soporte MULTIPLES SEGMENTOS (Opcion B).  Hasta entonces, el
        // envolvente es la aproximacion conservadora.
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
    // --- Instrumentacion de correctitud cross-call (donde cae cada valor vivo a
    // traves de un CALL).  linear_scan de PRODUCCION fuerza callee-saved/spill; el
    // modelo aun NO consume crosses_call -> aqui se ven caer en caller-saved.  Un
    // xcall_caller > 0 = asignacion INCORRECTA (el CALL clobbea la lane volatil).
    uint32_t xcall_values    = 0; ///< valores con crosses_call.
    uint32_t xcall_caller    = 0; ///< de esos, en lane VOLATILE (caller-saved) = PELIGRO.
    uint32_t xcall_callee    = 0; ///< de esos, en lane PRESERVED (callee-saved) = seguro.
    uint32_t xcall_spilled   = 0; ///< de esos, spilled (seguro).
    // NOTA: la CORRECTITUD (¿es un coloreo propio?) NO vive aqui -- ShadowStats son
    // METRICAS DE CALIDAD (spills/coste/presion).  El veredicto de correctitud es otro
    // plano y vive en ShadowReport (valid_linear/valid_rbank).
};

/**
 * @brief Clasifica un valor cross-call por el SavePolicy de su lane fisica.
 *        @p lane_id = MReg id (o @c kSpilled).  Suma en las cuentas xcall_*.
 */
inline void classify_xcall(ShadowStats &s, const OptimizationContext &ctx,
                           int lane_id, ViewWidth w) {
    ++s.xcall_values;
    if (lane_id == kSpilled) { ++s.xcall_spilled; return; }
    const SavePolicy sp = ctx.get_bank().preservation(static_cast<uint8_t>(lane_id), w);
    if (sp == SavePolicy::PRESERVED) ++s.xcall_callee;
    else                             ++s.xcall_caller; // VOLATILE (o RESERVED anomalo).
}

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
        const bool has = v.value_id < ra.assign.size();
        const bool spilled = has && ra.assign[v.value_id].loc == jit::RegAlloc::Loc::SPILL;
        const bool inreg   = has && ra.assign[v.value_id].loc == jit::RegAlloc::Loc::REG;
        if (spilled) {
            ++s.spills;
            s.spill_cost += spill_cost_via_objective(v.req, ctx);
        } else if (inreg) {
            if (is_gp) ++s.allocated_gp; else ++s.allocated_fp;
        }
        if (v.req.crosses_call && ctx.has_bank())
            classify_xcall(s, ctx, spilled ? kSpilled : (inreg ? (int)ra.assign[v.value_id].reg : kSpilled),
                           v.req.width);
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
                                      bool vec_active,
                                      LaneAssignment *out_assign = nullptr,
                                      AbstractProblem *out_coalesced = nullptr) {
    ShadowStats s;
    fill_pressure(p, s);
    const CoalesceResult co = coalesce_conservative(p);
    s.copies_removed = co.copies_eliminated;
    const LaneAssignment la = color_smart_spill(co.problem, ctx, vec_active);
    s.values = static_cast<uint32_t>(co.problem.values.size());
    s.spills = spill_count(co.problem, la);
    s.spill_cost = total_spill_cost(co.problem, la, ctx);
    // Expone la asignacion + el problema coalescido para que el llamante valide la
    // CORRECTITUD (is_proper_coloring) sin re-correr el pipeline -- la correctitud es
    // otro plano (ShadowReport), no una metrica.
    if (out_assign) *out_assign = la;
    if (out_coalesced) *out_coalesced = co.problem;
    // allocated cuenta LANES FISICAS ASIGNABLES.  Hoy "lane != spilled" <=> registro
    // fisico (el unico recurso no-memoria del banco), pero el dia que existan scratch
    // / stack-cache / pseudo-lanes / memoria persistente como recursos, "no spilled"
    // dejara de implicar "en registro" -> habra que distinguir por tipo de lane.
    for (const AbstractValue &v : co.problem.values) {
        if (v.req.fixed_reg >= 0) ++s.pinned_values;
        const int lane = la.lane_of(v.value_id);
        if (v.req.crosses_call && ctx.has_bank())
            classify_xcall(s, ctx, lane, v.req.width);
        if (lane == kSpilled) continue;
        if (v.req.cls == ResourceClass::GP) ++s.allocated_gp; else ++s.allocated_fp;
    }
    return s;
}

/**
 * @brief Convierte un @c RegAlloc de PRODUCCION a un @c LaneAssignment del modelo,
 *        para poder pasar la asignacion de linear_scan por @c is_proper_coloring
 *        (control: ¿el modelo considera propio lo que el backend YA acepta?).
 */
inline LaneAssignment regalloc_to_lanes(const jit::RegAlloc &ra,
                                        const AbstractProblem &p) {
    LaneAssignment la;
    for (const AbstractValue &v : p.values) {
        if (v.value_id < ra.assign.size() &&
            ra.assign[v.value_id].loc == jit::RegAlloc::Loc::REG)
            la.assign(v.value_id, ra.assign[v.value_id].reg);
        else
            la.spill(v.value_id);
    }
    return la;
}

/**
 * @struct ShadowReport
 * @brief Veredicto de UNA funcion.  Dos PLANOS separados:
 *          - CALIDAD: @c linear / @c rbank stats + @c diff + @c equivalent / @c improved.
 *          - CORRECTITUD: @c valid_linear / @c valid_rbank (¿coloreo propio del modelo?).
 *        No se mezclan: una asignacion puede ser mejor en spills y a la vez invalida.
 *
 * IMPORTANTE (oraculo): @c valid_rbank valida el MODELO (is_proper_coloring sobre el
 * problema abstracto), NO el backend.  El backend tiene informacion que el modelo aun
 * no representa (huecos del LiveRange, GC maps, uses, kill positions).  Por eso el
 * switch NO se decide solo con @c valid_rbank: exige ademas convertir la asignacion a
 * RegAlloc y pasarla por TODO el backend (rewrite -> encode -> diff_harness -> e2e).
 * @c valid_rbank es la senyal TEMPRANA de correctitud, no la prueba final.
 */
struct ShadowReport {
    ShadowStats linear;
    ShadowStats rbank;
    ShadowDiff  diff;
    bool        equivalent = false; ///< calidad: ni mejora ni empeora.
    bool        improved   = false; ///< calidad: mejora sin empeorar.
    // Correctitud DESGLOSADA (otro plano): por que falla, no solo si falla.
    ColoringValidation valid_linear; ///< control: linear como coloreo del modelo.
    ColoringValidation valid_rbank;  ///< senyal: rbank como coloreo del modelo.
};

/**
 * @brief Compara dos stats -> ShadowReport (calidad).  Las valideces (correctitud,
 *        desglosadas por categoria) se pasan aparte porque son otro plano; por
 *        defecto vacias (ok) para compat con tests.
 */
inline ShadowReport make_shadow_report(const ShadowStats &lin, const ShadowStats &rb,
                                       ColoringValidation valid_linear = {},
                                       ColoringValidation valid_rbank = {}) {
    ShadowReport r;
    r.linear = lin;
    r.rbank = rb;
    r.diff = shadow_diff(lin, rb);
    r.valid_linear = valid_linear;
    r.valid_rbank = valid_rbank;
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
    // Instrumentacion cross-call agregada (correctitud): cuantos valores vivos a
    // traves de un CALL caen en caller-saved (PELIGRO) con cada allocator.
    uint64_t xcall_values       = 0;
    uint64_t linear_xcall_caller = 0;
    uint64_t rbank_xcall_caller  = 0;
    uint64_t linear_xcall_callee = 0;
    uint64_t rbank_xcall_callee  = 0;
    uint32_t rbank_invalid       = 0; ///< funciones donde rbank NO fue coloreo propio (modelo).
    uint32_t linear_invalid      = 0; ///< control: linear no fue propio SEGUN el modelo.
    ColoringValidation rbank_errors;  ///< errores rbank por CATEGORIA (que falla).
    ColoringValidation linear_errors; ///< errores linear por CATEGORIA (control).

    /** @brief Acumula el veredicto de una funcion. */
    void add(const ShadowReport &r) {
        ++functions;
        if (!r.valid_rbank.ok()) ++rbank_invalid;
        if (!r.valid_linear.ok()) ++linear_invalid;
        rbank_errors.add(r.valid_rbank);
        linear_errors.add(r.valid_linear);
        if (r.improved) ++improved;
        else if (r.equivalent) ++equal;
        else ++worsened;
        linear_spills += r.linear.spills;
        rbank_spills += r.rbank.spills;
        linear_spill_cost += r.linear.spill_cost;
        rbank_spill_cost += r.rbank.spill_cost;
        xcall_values += r.linear.xcall_values; // mismo problema -> mismo conteo.
        linear_xcall_caller += r.linear.xcall_caller;
        rbank_xcall_caller += r.rbank.xcall_caller;
        linear_xcall_callee += r.linear.xcall_callee;
        rbank_xcall_callee += r.rbank.xcall_callee;
    }
};

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_SHADOW_H
