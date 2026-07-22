/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/measure.h
 * @brief INSTRUMENTO de medicion (el "shadow" de esta fase): mide, SIN tocar el
 *        backend, el POTENCIAL de dos mejoras del asignador para decidir CUAL paga
 *        el alquiler primero (medir antes de construir):
 *
 *   (a) spills EVITABLES por rematerializacion: de los valores que rbank derrama,
 *       cuantos son RECOMPUTABLES (RematFacts) -> se podrian recomputar en el uso
 *       en vez de reload.  Combina IR (recomputable) con la asignacion real.
 *   (b) FALSAS INTERFERENCIAS del envolvente: pares que el modelo actual marca
 *       interferentes usando el ENVOLVENTE [start,end], pero cuyos segmentos REALES
 *       (con huecos) NO solapan.  Cada una es presion inventada -> spills de mas.
 *
 * Es diagnostico: NO cambia el codigo emitido.  El numero decide si el primer
 * peldano con impacto es remat (a) o cerrar el envolvente (b).
 */

#ifndef VESTA_CODEGEN_RBANK_MEASURE_H
#define VESTA_CODEGEN_RBANK_MEASURE_H

#include "analysis/facts/remat_facts.h"
#include "jit/interval.h"
#include "jit/linear_scan.h"

#include <cstdint>

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
inline RematMeasure measure_remat(const jit::RegAlloc &ra,
                                  const analysis::RematFacts &remat) {
    RematMeasure m;
    for (uint32_t v = 0; v < ra.assign.size(); ++v) {
        if (ra.assign[v].loc != jit::RegAlloc::Loc::SPILL) continue;
        ++m.spills_total;
        if (!remat.is_rematerializable(v)) continue;
        ++m.spills_rematerializable;
        if (remat.recipe_of(v).operands.empty()) ++m.spills_remat_leaf;
    }
    return m;
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

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_MEASURE_H
