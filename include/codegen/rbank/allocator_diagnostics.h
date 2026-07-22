/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/allocator_diagnostics.h
 * @brief AllocatorDiagnostics: TELEMETRIA / DIAGNOSTICO de EJECUCION del allocator.
 *
 * TERCER NIVEL de conocimiento, independiente de los otros dos:
 *   - IR Facts        -> describen el PROGRAMA   (UseDefFacts, LoopFacts, RematFacts).
 *   - HW Facts        -> describen la MAQUINA     (MachineCostFacts, SpillCostCard).
 *   - AllocatorDiagnostics -> describen el COMPORTAMIENTO del algoritmo de asignacion
 *                        sobre un problema concreto.  NO es un "hecho del programa":
 *                        es el RESULTADO DE EJECUTAR un algoritmo.  Por eso NO lleva
 *                        el sufijo @c Facts ni vive en @c analysis/ -- seria
 *                        contaminar el motor de conocimiento con telemetria del
 *                        backend.  Y NO es "solo presion": mide pico, desperdicio,
 *                        spills, reutilizacion... y mañana split/recolor/fragmentacion
 *                        /affinity -- todo es DIAGNOSTICO del allocator.
 *
 * REUTILIZABLE por CUALQUIER allocator: se produce desde @c (IntervalResult +
 * codegen::RegAlloc + lanes) -- el problema y su asignacion -- sin conocer el algoritmo.
 * Da un lenguaje OBJETIVO para comparar greedy / graph-coloring / PBQP / Belady y
 * responder "¿el allocator derrama porque DEBE o porque su estrategia es ineficiente?".
 *
 * EL CAMPO CLAVE es @c peak_idle_capacity: pregunta BINARIA -- ¿habia una lane libre
 * mientras un spill vivo podia ocuparla?  @c ==0 => el algoritmo respeta el invariante;
 * @c >0 => deja registros VACIOS con valores en memoria -> bug conceptual, no heuristica.
 *
 * i18n: produce DATOS.  ADITIVO, diagnostico (no cambia el codigo emitido).
 */

#ifndef VESTA_CODEGEN_RBANK_ALLOCATOR_DIAGNOSTICS_H
#define VESTA_CODEGEN_RBANK_ALLOCATOR_DIAGNOSTICS_H

#include "jit/interval.h"
#include "jit/linear_scan.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @struct AllocatorDiagnostics
 * @brief Telemetria del allocator: vocabulario en el PICO + senales de desperdicio.
 *        Producido por @c compute_allocator_diagnostics.
 */
struct AllocatorDiagnostics {
    // === Vocabulario en el PICO de presion (el instante critico) ===
    uint32_t peak_position    = 0; ///< posicion (dominio MachineIR) del pico.
    uint32_t live_values      = 0; ///< vregs GP vivos en el pico.
    uint32_t occupied_lanes   = 0; ///< de esos, en REGISTRO (ocupan una lane).
    uint32_t free_lanes       = 0; ///< lanes - occupied: LIBRES en el pico.
    uint32_t spilled_in_peak  = 0; ///< de los vivos en el pico, cuantos en SPILL.
    /// EL CAMPO CLAVE: spills del pico que CABRIAN en free_lanes = min(free, spilled).
    /// ==0 invariante respetado; >0 el allocator deja registros vacios (bug conceptual).
    uint32_t peak_idle_capacity = 0;

    // === Diagnostico del ALGORITMO (comportamiento, no programa) ===
    uint32_t max_wasted_lanes = 0; ///< peor punto: min(lanes libres, vregs-spill vivos).
    uint64_t points_wasting   = 0; ///< posiciones (eventos) con lane libre Y spill vivo.
    /// AREA de desperdicio: Σ min(free_lanes, spill_live) * duracion del tramo, sobre
    /// TODA la funcion -- "cuanto TIEMPO estuvieron registros sin trabajar".  Mucho mas
    /// informativo que contar posiciones: un pico perfecto pero 500 posiciones con una
    /// lane libre tambien cuesta.  Objetivo del arreglo del greedy: -> 0.
    uint64_t wasted_lane_area = 0;
    uint32_t spilled_total    = 0; ///< vregs con loc=SPILL en toda la funcion.
    // (2) SPILLS DEL PICO: @c spilled_in_peak vs @c spilled_total.  Iguales => los N
    //     spills son DEL MISMO pico; spilled_in_peak << spilled_total => picos distintos.
    // spill_churn (spill..reload..spill por vreg): en linear-scan SIN splitting cada
    // vreg tiene UNA loc para toda su vida -> == 0 por construccion.  Su ausencia YA
    // informa: "derrama de mas", no "politica de reutilizacion".
    //
    // DIRECCION (avoidable_spill_cost): el numero de spills evitables se convierte en
    // COSTE fusionando los tres niveles -- Σ execution_weight (IR: LoopFacts/Profile)
    // x reload_cost (HW: SpillCostCard) de los spills con peak_idle_capacity>0.  Dice
    // "el greedy pierde ~X ciclos aqui".  Requiere cablear ValueRequirements + la card
    // al diagnostico (hoy solo tiene ivs + ra); se anade al atacar la palanca (3).

    /// Agregado del corpus: conserva el BLOQUE del pico de la peor funcion (mayor
    /// @c live_values) y acumula/maximiza las senales de desperdicio.
    void add(const AllocatorDiagnostics &o) noexcept {
        const uint64_t pw = points_wasting + o.points_wasting;
        const uint64_t wa = wasted_lane_area + o.wasted_lane_area;
        const uint32_t mw = std::max(max_wasted_lanes, o.max_wasted_lanes);
        const uint32_t st = std::max(spilled_total, o.spilled_total);
        if (o.live_values > live_values) *this = o; // peor funcion completa (coherente).
        points_wasting   = pw;
        wasted_lane_area = wa;
        max_wasted_lanes = mw;
        spilled_total    = st;
    }
};

/**
 * @brief Produce @c AllocatorDiagnostics de una asignacion: sweep de los segmentos GP
 *        (numeracion de @c build_intervals) separando REG de SPILL segun @p ra, para
 *        localizar el pico, el desperdicio puntual y el AREA de lanes ociosas.
 *        @p lanes = registros GP asignables.
 */
inline AllocatorDiagnostics compute_allocator_diagnostics(const jit::IntervalResult &ivs,
                                                          const codegen::RegAlloc &ra,
                                                          uint32_t lanes) {
    struct Ev { uint32_t pos; int delta; bool spill; };
    std::vector<Ev> ev;
    uint32_t spilled_total = 0;
    for (const jit::LiveInterval &iv : ivs.intervals) {
        if (iv.ranges.empty() || iv.cls != jit::RegClass::GP) continue;
        if (iv.vreg >= ra.assign.size()) continue;
        const bool spill = ra.assign[iv.vreg].loc == codegen::RegAlloc::Loc::SPILL;
        if (spill) ++spilled_total;
        for (const jit::LiveRange &r : iv.ranges) {
            ev.push_back({r.from, +1, spill});
            ev.push_back({r.to, -1, spill});
        }
    }
    std::sort(ev.begin(), ev.end(), [](const Ev &a, const Ev &b) {
        return a.pos != b.pos ? a.pos < b.pos : a.delta < b.delta; // salidas antes.
    });
    AllocatorDiagnostics d;
    d.spilled_total = spilled_total;
    int cur_reg = 0, cur_spill = 0;
    uint32_t prev_pos = 0;
    bool have_prev = false;
    size_t i = 0;
    while (i < ev.size()) {
        const uint32_t p = ev[i].pos;
        // AREA del tramo [prev_pos, p): el estado (cur_reg,cur_spill) vale ahi.
        if (have_prev && p > prev_pos) {
            const int libres = static_cast<int>(lanes) - cur_reg;
            if (libres > 0 && cur_spill > 0)
                d.wasted_lane_area +=
                    static_cast<uint64_t>(std::min(libres, cur_spill)) * (p - prev_pos);
        }
        while (i < ev.size() && ev[i].pos == p) { // aplica los eventos de p.
            if (ev[i].spill) cur_spill += ev[i].delta;
            else             cur_reg += ev[i].delta;
            ++i;
        }
        const int live = cur_reg + cur_spill;
        if (live > static_cast<int>(d.live_values)) { // nuevo pico.
            d.peak_position     = p;
            d.live_values       = static_cast<uint32_t>(live);
            d.occupied_lanes    = static_cast<uint32_t>(cur_reg);
            d.free_lanes        = cur_reg < static_cast<int>(lanes)
                                      ? lanes - static_cast<uint32_t>(cur_reg) : 0u;
            d.spilled_in_peak   = static_cast<uint32_t>(cur_spill);
            d.peak_idle_capacity = std::min(d.free_lanes, d.spilled_in_peak);
        }
        const int libres = static_cast<int>(lanes) - cur_reg;
        if (libres > 0 && cur_spill > 0) {
            const uint32_t wasted = static_cast<uint32_t>(std::min(libres, cur_spill));
            if (wasted > d.max_wasted_lanes) d.max_wasted_lanes = wasted;
            ++d.points_wasting;
        }
        prev_pos = p;
        have_prev = true;
    }
    return d;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_ALLOCATOR_DIAGNOSTICS_H
