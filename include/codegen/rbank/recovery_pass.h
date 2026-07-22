/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/recovery_pass.h
 * @brief RECOVERY PASS: 2a pasada tras el linear-scan que REPARA la asignacion
 *        INCOMPLETA del greedy -- reasigna a REGISTRO los vregs derramados para los
 *        que existe una lane LIBRE durante todo su intervalo.
 *
 *     color_smart_spill (greedy)            -> LaneAssignment (con spills de mas)
 *              |                                que dejan lanes ociosas
 *     recover_spills  (esta pasada)          -> reasigna esos spills a lanes libres
 *              |
 *     regalloc_from_lanes                    -> codegen::RegAlloc
 *
 * POR QUE.  @c AllocatorDiagnostics midio que el greedy viola el invariante
 * "lanes_libres>0 && spills_vivos>0" (asignacion incompleta): marca un vreg SPILL
 * en el pico y lo deja spill TODA su vida aunque luego se liberen lanes.  Esta
 * pasada NO cambia el algoritmo: toma la asignacion hecha y, para cada spill,
 * busca una lane que este libre en TODO su intervalo (una lane libre EN EL PICO lo
 * esta siempre, pues el pico es la maxima ocupacion) y lo recupera.  Cuantifica
 * cuanto del gap era "algoritmo" (recuperable aqui) vs "representacion" (necesita
 * splitting para los spills que caben FUERA del pico pero no en el).
 *
 * CORRECTITUD.  Solo reasigna a una lane que @c lane_admissible acepta para el vreg
 * (misma clase, no forbidden, callee-saved si cruza CALL, respeta fixed_reg y
 * must_be_memory) Y que no solapa (envolvente + aliasing) con ningun valor ya
 * asignado ahi.  Conservadora (usa el envolvente [start,end], que sobre-estima la
 * vida): nunca crea conflicto; a lo sumo deja pasar oportunidades que exigirian
 * rangos exactos.  El diagnostico es el ORACULO: donde el no ve @c candidate>0, esta
 * pasada tampoco encuentra hueco.
 *
 * i18n: produce DATOS (numero de recuperados).  ADITIVO.
 */

#ifndef VESTA_CODEGEN_RBANK_RECOVERY_PASS_H
#define VESTA_CODEGEN_RBANK_RECOVERY_PASS_H

#include "codegen/rbank/abstract_problem.h"
#include "codegen/rbank/allowed_lanes.h"
#include "codegen/rbank/physical_bank.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @brief Recupera a registro los vregs derramados que caben en una lane libre en
 *        todo su intervalo.  Modifica @p la in-place.  @return numero de spills
 *        recuperados (para instrumentacion).
 */
inline uint32_t recover_spills(const AbstractProblem &p, LaneAssignment &la,
                               const PhysicalRegisterBank &bank, bool vec_active) {
    // Ocupacion por lane fisica (indexada por id): intervalos [start,end] envolventes
    // de los valores ya asignados a esa lane.  Array contiguo (no map): pocos ids.
    uint32_t max_id = 0;
    for (const Lane &l : bank.lanes)
        if (l.id > max_id) max_id = l.id;
    struct Occ { uint32_t start, end; };
    std::vector<std::vector<Occ>> occ(static_cast<size_t>(max_id) + 1);
    for (const AbstractValue &v : p.values) {
        const int lane = la.lane_of(v.value_id);
        if (lane != kSpilled && lane >= 0 && static_cast<uint32_t>(lane) <= max_id)
            occ[static_cast<size_t>(lane)].push_back({v.start, v.end});
    }

    uint32_t recovered = 0;
    for (const AbstractValue &v : p.values) {
        if (la.lane_of(v.value_id) != kSpilled) continue;
        for (const Lane &l : bank.lanes) {
            if (!lane_admissible(v.req, l, vec_active)) continue; // correctitud dura.
            // ¿Libre en [v.start, v.end] en l y en las lanes que aliasan con l?
            const AliasSet *ls = bank.aliases_of(l.id);
            bool free_all = true;
            for (uint32_t id = 0; id <= max_id && free_all; ++id) {
                if (occ[id].empty()) continue;
                const AliasSet *os = bank.aliases_of(static_cast<uint8_t>(id));
                if (!ls || !os || !ls->overlaps(*os)) continue; // no aliasa con l.
                for (const Occ &o : occ[id])
                    if (v.start <= o.end && o.start <= v.end) { free_all = false; break; }
            }
            if (free_all) {
                la.assign(v.value_id, l.id);           // recuperado: spill -> registro.
                occ[l.id].push_back({v.start, v.end}); // ocupa la lane desde ya.
                ++recovered;
                break;
            }
        }
    }
    return recovered;
}

/**
 * @struct SpillTaxonomy
 * @brief Clasifica CADA spill por QUE existe -- el "mapa" que dirige el siguiente
 *        escalon del allocator y cierra el modelo explicativo:
 *   - fully: hay una lane admisible libre en TODO su intervalo -> lo recupera la
 *     Recovery Pass (recuperacion completa).
 *   - partially: no fully, pero hay una lane libre en ALGUN subintervalo -> hueco
 *     PARCIAL; dominio del Fragmentation Recovery (splitting).
 *   - structural: en NINGUN punto de su intervalo hay lane admisible libre ->
 *     inevitable (overflow, fisica).
 * spilled_total = structural + fully + partially.
 */
struct SpillTaxonomy {
    uint32_t fully      = 0;
    uint32_t partially  = 0;
    uint32_t structural = 0;
    // splitting_potential = TECHO del splitting: para cada partially, la MAXIMA
    // duracion libre en una lane admisible (tiempo que ese spill PODRIA estar en
    // registro en vez de memoria).  Es el wasted_lane_area recuperable si una
    // Fragmentation Recovery ideal aprovechara todos los huecos disponibles -- se mide
    // ANTES de implementarla (medir el techo, como fully lo fue de la Recovery).
    // Cuantifica partially: 954 spills no dicen si son de longitud 1 o 400; esto sí.
    //
    // METODOLOGIA del backend (no solo del allocator): toda optimizacion nace con el
    // mismo esquema, para dirigirse con datos y no con intuiciones.  REGLA DE ORO:
    //
    //     La Taxonomia describe el PROBLEMA (que spills existen y por que).
    //     Potential describe el TECHO      (cuanto se podria mejorar, medido antes).
    //     Recovered mide la IMPLEMENTACION (cuanto mejora de verdad la transformacion).
    //     Remaining decide el SIGUIENTE SPRINT (cuanto margen queda para algo mas potente).
    //
    //     Potential  ->  Recovered  ->  Remaining
    //
    //   Potential = techo teorico MEDIDO antes de implementar (¿cuanto podria mejorar?).
    //   Recovered = mejora real de la transformacion (¿cuanto mejora de verdad?).
    //   Remaining = margen residual para una transformacion mas potente.
    //
    // Asi cada optimizacion (Recovery, Splitting, Remat, Vectorization, Coalescing...)
    // nace con un criterio OBJETIVO de cuando deja de merecer inversion, y todas son
    // comparables entre si.  Recovery ya lo sigue (Fully/Recovered/Potential); el
    // splitting anyadira recovered_area/remaining_area cuando exista.
    uint64_t splitting_potential = 0;

    void add(const SpillTaxonomy &o) noexcept {
        fully += o.fully;
        partially += o.partially;
        structural += o.structural;
        splitting_potential += o.splitting_potential;
    }
};

/**
 * @brief Clasifica los spills de @p la en fully/partially/structural.  @c fully es
 *        una PROPIEDAD DEL GRAFO (existe >=1 lane admisible libre en TODO el
 *        intervalo del spill AISLADO), NO lo que la Recovery greedy recupera: tres
 *        spills pueden ser fully y compartir la misma lane -> solo 1 recuperable, sin
 *        contradiccion.  @c fully es el LIMITE SUPERIOR; el greedy y el matching
 *        optimo (futuro, bipartito spills-lanes) recuperan <= fully.
 *          - fully: existe una lane admisible libre en TODO su intervalo (arista).
 *          - partially: no fully, pero alguna lane admisible tiene un HUECO
 *            (subintervalo libre) -> objetivo del Fragmentation Recovery (splitting).
 *          - structural: ninguna lane admisible libre en ningun punto -> inevitable.
 *        Nota: structural (per-spill: sin hueco NUNCA) != overflow_exact (per-pico:
 *        presion del instante).  DIAGNOSTICO (no modifica @p la).
 */
inline SpillTaxonomy classify_spills(const AbstractProblem &p, const LaneAssignment &la,
                                     const PhysicalRegisterBank &bank, bool vec_active) {
    uint32_t max_id = 0;
    for (const Lane &l : bank.lanes)
        if (l.id > max_id) max_id = l.id;
    struct Occ { uint32_t start, end; };
    std::vector<std::vector<Occ>> occ(static_cast<size_t>(max_id) + 1);
    for (const AbstractValue &v : p.values) {
        const int lane = la.lane_of(v.value_id);
        if (lane != kSpilled && lane >= 0 && static_cast<uint32_t>(lane) <= max_id)
            occ[static_cast<size_t>(lane)].push_back({v.start, v.end});
    }

    SpillTaxonomy t;
    std::vector<Occ> hits; // scratch reusado.
    for (const AbstractValue &v : p.values) {
        if (la.lane_of(v.value_id) != kSpilled) continue;
        const uint64_t total = static_cast<uint64_t>(v.end) - v.start + 1;
        uint64_t max_free = 0;  // maxima duracion libre sobre lanes admisibles.
        bool any_full = false;
        for (const Lane &l : bank.lanes) {
            if (!lane_admissible(v.req, l, vec_active)) continue;
            const AliasSet *ls = bank.aliases_of(l.id);
            hits.clear();
            for (uint32_t id = 0; id <= max_id; ++id) {
                if (occ[id].empty()) continue;
                const AliasSet *os = bank.aliases_of(static_cast<uint8_t>(id));
                if (!ls || !os || !ls->overlaps(*os)) continue; // no aliasa con l.
                for (const Occ &o : occ[id])
                    if (v.start <= o.end && o.start <= v.end)
                        hits.push_back(
                            {std::max(o.start, v.start), std::min(o.end, v.end)});
            }
            if (hits.empty()) { any_full = true; max_free = total; break; } // 100% libre.
            std::sort(hits.begin(), hits.end(),
                      [](const Occ &a, const Occ &b) { return a.start < b.start; });
            // Union de los hits recortados a [v.start, v.end] -> tiempo OCUPADO; el
            // libre (= total - ocupado) es lo que el splitting podria dar a registro.
            uint64_t covered = 0;
            uint32_t cur_s = hits[0].start, cur_e = hits[0].end;
            for (size_t i = 1; i < hits.size(); ++i) {
                if (hits[i].start <= cur_e + 1) {
                    if (hits[i].end > cur_e) cur_e = hits[i].end; // solapa/contiguo: fusiona.
                } else {
                    covered += static_cast<uint64_t>(cur_e) - cur_s + 1;
                    cur_s = hits[i].start;
                    cur_e = hits[i].end;
                }
            }
            covered += static_cast<uint64_t>(cur_e) - cur_s + 1;
            const uint64_t free_len = total - covered; // covered <= total (recortado).
            if (free_len > max_free) max_free = free_len;
        }
        if (any_full) ++t.fully;
        else if (max_free > 0) { ++t.partially; t.splitting_potential += max_free; }
        else ++t.structural;
    }
    return t;
}

// NOTA (2026): el "optimo de Recovery" NO es un matching bipartito spills->lanes.
// Un matching asume <=1 spill por lane, pero @c recover_spills (greedy) aloja VARIOS
// intervalos no-solapantes en la misma lane -> el bipartito SUBESTIMA (medido en el
// corpus: matching=106 < greedy=134).  El modelo correcto es INTERVAL SCHEDULING con
// lanes (varios intervalos no-solapantes por maquina).  Se deja fuera a proposito: la
// taxonomia del corpus muestra partially=62.5% >> fully=14.2%, asi que el margen esta
// en el SPLITTING (Fragmentation Recovery), no en perfeccionar la Recovery de fully.
// El KPI honesto es: Fully(limite superior) / Recovered(greedy) / Potential=Fully-Rec.

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_RECOVERY_PASS_H
