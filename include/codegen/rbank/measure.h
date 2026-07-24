/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/measure.h
 * @brief INSTRUMENTO de medicion: mide, SIN tocar el backend, el POTENCIAL de
 *        mejoras del asignador para decidir CUAL paga el alquiler primero (medir
 *        antes de construir).  Principio: el allocator NO derrama por un PAR sino
 *        por PRESION; por eso el instrumento decisivo es (c) el pico de presion.
 *
 *   (a) spills EVITABLES por rematerializacion: de los valores que rbank derrama,
 *       cuantos son RECOMPUTABLES (RematFacts) -> se podrian recomputar en el uso
 *       en vez de reload.  Combina IR (recomputable) con la asignacion real.
 *   (b) FALSAS INTERFERENCIAS del envolvente: pares que el modelo marca
 *       interferentes por el ENVOLVENTE [start,end] pero cuyos segmentos REALES
 *       (con huecos) NO solapan.  Cada una es presion inventada -- PROXY de (c).
 *   (c) PICO DE PRESION envolvente vs exacto: el maximo de intervalos vivos a la
 *       vez por clase.  El allocator derrama cuando el pico supera los lanes; el
 *       DELTA (env - exact) dice si cerrar el envolvente quita spills REALES o
 *       ninguno.  A diferencia de (b) -- que cuenta pares que quiza nunca
 *       coinciden en el tiempo -- (c) mira el instante de MAXIMA presion.
 *
 * Es diagnostico: NO cambia el codigo emitido.  El numero decide el peldano real.
 *
 * DIRECCIONES (documentadas, no ahora):
 *   - COSTE, no valores: (a) cuenta valores recomputables, pero el objetivo ultimo
 *     son CICLOS recuperables (estimated_reload vs estimated_remat via
 *     MachineCostFacts).  Separar "es recomputable" de "merece la pena" cuando el
 *     coste HW entre en la ecuacion.
 *   - GENERICO: el patron (optimizacion -> instrumento que mide -> decision) sirve
 *     igual a scheduler / vectorizer / inlining / LICM / GVN.  Cuando exista un
 *     segundo consumidor, extraer el molde a un sitio comun (hoy es de rbank).
 */

#ifndef VESTA_CODEGEN_RBANK_MEASURE_H
#define VESTA_CODEGEN_RBANK_MEASURE_H

#include "analysis/facts/remat_facts.h"
#include "codegen/rbank/value_requirements.h"
#include "jit/interval.h"
#include "codegen/regalloc.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @struct RematMeasure
 * @brief (a) De los spills de una funcion, cuantos son recomputables.  El mapeo
 *        vreg <-> IrValueId es 1:1 para los valores SSA (los temporales del
 *        selector caen fuera de RematFacts -> no recomputables, conservador).
 */
struct RematMeasure {
    uint32_t spills_total          = 0; ///< valores derramados por rbank.
    uint32_t spills_rematerializable = 0; ///< de esos, recomputables (techo bruto).
    uint32_t spills_remat_leaf     = 0; ///< de esos, HOJA (0 operandos: CONST/dir) ->
                                        ///< recompute puro, ganancia casi garantizada.

    void add(const RematMeasure &o) noexcept {
        spills_total += o.spills_total;
        spills_rematerializable += o.spills_rematerializable;
        spills_remat_leaf += o.spills_remat_leaf;
    }
};

/** @brief (a) Mide sobre la asignacion de rbank cuantos spills son recomputables,
 *         separando los HOJA (sin operandos: recompute puro, ganancia casi segura)
 *         de los que necesitan mantener operandos vivos (ganancia condicional al coste). */
inline RematMeasure measure_remat(const codegen::RegAlloc &ra,
                                  const analysis::RematFacts &remat) {
    RematMeasure m;
    for (uint32_t v = 0; v < ra.assign.size(); ++v) {
        if (ra.assign[v].loc != codegen::RegAlloc::Loc::SPILL) continue;
        ++m.spills_total;
        if (!remat.is_rematerializable(v)) continue;
        ++m.spills_rematerializable;
        if (remat.recipe_of(v).operands.empty()) ++m.spills_remat_leaf;
    }
    return m;
}

/**
 * @struct RematDetail
 * @brief Dirige el esfuerzo al NIVEL correcto (no "¿construyo?" -- el Fact ya vale):
 *          - HOTNESS: spills recomputables DENTRO de un loop (pagan) vs frios (ruido).
 *          - RAZON de que el CONST llegue como valor separado y se derrame:
 *            imm que CABE en 32 bits -> deberia fusionarse como immediate AGUAS ARRIBA
 *            (selector, nivel mas alto); imm64 -> genuinamente necesita remat/mov imm64.
 */
struct RematDetail {
    uint32_t spills_in_loop = 0; ///< META: TODOS los spills con loop_depth > 0 (hot).
    uint32_t spills_cold    = 0; ///< META: todos los spills fuera de loop.
    uint32_t remat_in_loop  = 0; ///< recomputables spilled con loop_depth > 0.
    uint32_t remat_cold     = 0; ///< recomputables spilled fuera de loop.
    uint32_t const_imm32    = 0; ///< CONST spilled con imm de 32 bits (fusionable arriba).
    uint32_t const_imm64    = 0; ///< CONST spilled con imm de 64 bits (necesita remat real).

    void add(const RematDetail &o) noexcept {
        spills_in_loop += o.spills_in_loop; spills_cold += o.spills_cold;
        remat_in_loop += o.remat_in_loop;   remat_cold += o.remat_cold;
        const_imm32 += o.const_imm32;       const_imm64 += o.const_imm64;
    }
};

/**
 * @brief Detalle de los spills recomputables: hotness + por que el CONST no se fusiono
 *        como immediate.  El instrumento (algoritmo) solo CONSULTA Facts, no los
 *        reinventa: la hotness por valor ya es un Fact (@c ValueRequirements.loop_depth,
 *        poblado desde LoopFacts) y el imm ya vive en @c RematFacts.recipe.  No itera
 *        el IR ni recomputa el loop_depth -- dato (Facts) separado del algoritmo.
 */
inline RematDetail measure_remat_detail(const codegen::RegAlloc &ra,
                                        const analysis::RematFacts &remat,
                                        const std::vector<ValueRequirements> &reqs) {
    RematDetail d;
    for (uint32_t v = 0; v < ra.assign.size(); ++v) {
        if (ra.assign[v].loc != codegen::RegAlloc::Loc::SPILL) continue;
        const bool hot = v < reqs.size() && reqs[v].loop_depth > 0; // Fact consultado.
        if (hot) ++d.spills_in_loop; else ++d.spills_cold;          // META: todos.
        if (!remat.is_rematerializable(v)) continue;
        if (hot) ++d.remat_in_loop; else ++d.remat_cold;
        const analysis::RematRecipe &r = remat.recipe_of(v);         // el imm del Fact.
        if (r.op == ir::IrOp::CONST) {
            const int64_t imm = static_cast<int64_t>(r.imm);
            if (imm >= INT32_MIN && imm <= INT32_MAX) ++d.const_imm32;
            else                                      ++d.const_imm64;
        }
    }
    return d;
}

/**
 * @struct EnvelopeMeasure
 * @brief (b) Interferencia por ENVOLVENTE vs por SEGMENTOS REALES.  Una falsa
 *        interferencia = par que el envolvente cree interferente pero cuyos rangos
 *        reales no solapan (huecos) -> presion inventada.
 */
struct EnvelopeMeasure {
    uint64_t pairs_envelope   = 0; ///< pares que INTERFIEREN por envolvente.
    uint64_t pairs_exact      = 0; ///< pares que INTERFIEREN por segmentos reales.
    uint64_t false_interfere  = 0; ///< envolvente SI, exacto NO (deuda del envolvente).

    void add(const EnvelopeMeasure &o) noexcept {
        pairs_envelope += o.pairs_envelope;
        pairs_exact += o.pairs_exact;
        false_interfere += o.false_interfere;
    }
};

/** @brief ¿Los envolventes [a.from,a.to) y [b.from,b.to) solapan? (semi-abierto). */
inline bool envelope_overlap(const jit::LiveInterval &a,
                             const jit::LiveInterval &b) noexcept {
    const uint32_t a0 = a.ranges.front().from, a1 = a.ranges.back().to;
    const uint32_t b0 = b.ranges.front().from, b1 = b.ranges.back().to;
    return a0 < b1 && b0 < a1;
}

/** @brief ¿Algun par de segmentos REALES de @p a y @p b solapa? (merge-walk O(n+m)). */
inline bool segments_overlap(const jit::LiveInterval &a,
                             const jit::LiveInterval &b) noexcept {
    size_t i = 0, j = 0;
    while (i < a.ranges.size() && j < b.ranges.size()) {
        const jit::LiveRange &ra = a.ranges[i];
        const jit::LiveRange &rb = b.ranges[j];
        if (ra.from < rb.to && rb.from < ra.to) return true; // solapan.
        if (ra.to <= rb.to) ++i; else ++j;                   // avanza el que acaba antes.
    }
    return false;
}

/**
 * @brief (b) Compara, por CLASE, la interferencia envolvente vs la exacta sobre los
 *        intervalos reales.  O(N^2) por clase (diagnostico; el grafo eficiente es
 *        otra cosa).  Solo intervalos no vacios; pares de la MISMA clase.
 */
inline EnvelopeMeasure measure_envelope(const jit::IntervalResult &ivs) {
    EnvelopeMeasure m;
    const auto &iv = ivs.intervals;
    for (size_t i = 0; i < iv.size(); ++i) {
        if (iv[i].ranges.empty()) continue;
        for (size_t j = i + 1; j < iv.size(); ++j) {
            if (iv[j].ranges.empty() || iv[j].cls != iv[i].cls) continue;
            const bool env = envelope_overlap(iv[i], iv[j]);
            if (!env) continue; // el envolvente no los cree interferentes.
            ++m.pairs_envelope;
            const bool exact = segments_overlap(iv[i], iv[j]);
            if (exact) ++m.pairs_exact;
            else       ++m.false_interfere; // envolvente SI, exacto NO.
        }
    }
    return m;
}

/**
 * @struct PressureMeasure
 * @brief (c) PICO de presion por clase: envolvente vs exacto.  El allocator NO
 *        derrama por un par sino por PRESION -- el maximo de intervalos vivos a la
 *        vez.  @c peak_env usa el envolvente [front, back); @c peak_exact usa los
 *        segmentos reales.
 *
 * La magnitud que decide NO es el pico crudo sino el OVERFLOW = max(0, peak -
 * lanes): un pico de 9 con 16 lanes no derrama nada; uno de 19 con 16 lanes
 * derrama 3.  Por eso los peaks se leen via @c overflow():
 *   - @c overflow(peak_exact, lanes) = spills FORZADOS por la presion real
 *     (minimo teorico; NINGUNA politica de victima los evita -- son estructurales).
 *   - @c avoidable = overflow(env) - overflow(exact) = spills que el ENVOLVENTE
 *     INVENTA; cota superior de lo que se gana cerrando el envolvente (segmentos).
 * Nota: el pico es una propiedad de los INTERVALOS (liveness), no de la
 * asignacion -- no cambia al mejorar la eleccion de victima (Belady); lo que esa
 * mejora reduce es el numero de spills REALES hacia @c overflow(exact).
 */
struct PressureMeasure {
    uint32_t peak_env_gp = 0, peak_exact_gp = 0; ///< pico GP (envolvente / exacto).
    uint32_t peak_env_fp = 0, peak_exact_fp = 0; ///< pico FP (envolvente / exacto).

    /// Agregado del corpus = PICO (max), no suma: el peor caso manda.
    void add(const PressureMeasure &o) noexcept {
        peak_env_gp   = std::max(peak_env_gp, o.peak_env_gp);
        peak_exact_gp = std::max(peak_exact_gp, o.peak_exact_gp);
        peak_env_fp   = std::max(peak_env_fp, o.peak_env_fp);
        peak_exact_fp = std::max(peak_exact_fp, o.peak_exact_fp);
    }

    /// Spills FORZADOS por presion = max(0, peak - lanes).  0 si @p lanes == 0
    /// (desconocido).  Es lo que decide el allocator, no el pico crudo.
    static uint32_t overflow(uint32_t peak, uint32_t lanes) noexcept {
        return (lanes && peak > lanes) ? peak - lanes : 0;
    }
    /// Spills que el ENVOLVENTE inventa en GP (cota sup. de evitables cerrandolo).
    uint32_t avoidable_gp(uint32_t lanes) const noexcept {
        return overflow(peak_env_gp, lanes) - overflow(peak_exact_gp, lanes);
    }
    /// Idem en FP.
    uint32_t avoidable_fp(uint32_t lanes) const noexcept {
        return overflow(peak_env_fp, lanes) - overflow(peak_exact_fp, lanes);
    }
};

/** @brief Pico de un sweep de eventos (pos, +-1).  Orden: en el mismo @c pos las
 *         SALIDAS (-1) antes que las ENTRADAS (+1) -> dos intervalos que se tocan
 *         en un punto ([a,b)[b,c)) NO cuentan como presion simultanea (semi-abierto). */
inline uint32_t peak_from_events(std::vector<std::pair<uint32_t, int>> &ev) noexcept {
    std::sort(ev.begin(), ev.end(),
              [](const std::pair<uint32_t, int> &a, const std::pair<uint32_t, int> &b) {
                  return a.first != b.first ? a.first < b.first : a.second < b.second;
              });
    int cur = 0, peak = 0;
    for (const std::pair<uint32_t, int> &e : ev) {
        cur += e.second;
        if (cur > peak) peak = cur;
    }
    return static_cast<uint32_t>(peak);
}

/**
 * @brief (c) Pico de presion por clase, envolvente vs exacto, via sweep de
 *        eventos.  O(N log N) por clase (diagnostico).  Solo intervalos no vacios.
 */
inline PressureMeasure measure_pressure(const jit::IntervalResult &ivs) {
    std::vector<std::pair<uint32_t, int>> env_gp, ex_gp, env_fp, ex_fp;
    for (const jit::LiveInterval &iv : ivs.intervals) {
        if (iv.ranges.empty()) continue;
        const bool gp = (iv.cls == jit::RegClass::GP);
        std::vector<std::pair<uint32_t, int>> &env = gp ? env_gp : env_fp;
        std::vector<std::pair<uint32_t, int>> &ex  = gp ? ex_gp : ex_fp;
        env.emplace_back(iv.ranges.front().from, +1); // envolvente [front, back).
        env.emplace_back(iv.ranges.back().to, -1);
        for (const jit::LiveRange &r : iv.ranges) {   // segmentos reales.
            ex.emplace_back(r.from, +1);
            ex.emplace_back(r.to, -1);
        }
    }
    PressureMeasure m;
    m.peak_env_gp   = peak_from_events(env_gp);
    m.peak_exact_gp = peak_from_events(ex_gp);
    m.peak_env_fp   = peak_from_events(env_fp);
    m.peak_exact_fp = peak_from_events(ex_fp);
    return m;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_MEASURE_H
