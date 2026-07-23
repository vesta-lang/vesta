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
#include "codegen/rbank/lane_occupancy.h"
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
 *
 * La ocupacion fisica la conoce @c LaneOccupancy (un solo sitio la define): aqui solo
 * queda la POLITICA -- "primera lane admisible libre en todo el intervalo".
 */
inline uint32_t recover_spills(const AbstractProblem &p, LaneAssignment &la,
                               const PhysicalRegisterBank &bank, bool vec_active) {
    LaneOccupancy occ = lane_occupancy_of(p, la, bank);
    uint32_t recovered = 0;
    for (const AbstractValue &v : p.values) {
        if (la.lane_of(v.value_id) != kSpilled) continue;
        const PosRange life{v.start, v.end};
        for (const Lane &l : bank.lanes) {
            if (!lane_admissible(v.req, l, vec_active)) continue; // correctitud dura.
            if (!occ.is_free(l.id, life)) continue;
            la.assign(v.value_id, l.id); // recuperado: spill -> registro.
            occ.occupy(l.id, life);      // ocupa la lane desde ya.
            ++recovered;
            break;
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
    // splitting_potential = TECHO ESTRUCTURAL del splitting: para cada partially, la
    // MAXIMA duracion libre en una lane admisible (tiempo que ese spill PODRIA estar en
    // registro en vez de memoria).  Se mide ANTES de implementar (igual que fully fue el
    // techo de la Recovery) y cuantifica partially: 954 spills no dicen si son de
    // longitud 1 o 400; esto si.
    //
    // QUE PREGUNTA RESPONDE (no es una cota "floja", es OTRA cota).  Responde:
    //
    //     ¿cuanto tiempo de lane libre EXISTE?
    //
    // y NO:
    //
    //     ¿cuanto tiempo de lane libre coincide con un uso susceptible de recuperarse?
    //
    // No descuenta ventanas SIN usos (donde no hay nada que ahorrar), ni la competencia
    // entre spills por la misma lane, ni las fronteras de bloque.  MEDIDO (2026): el
    // splitting recupera 33190 de 241372 y el Remaining resultante (209058) convive con
    // apenas unos miles de posiciones realmente candidatas -- ese Remaining NO es trabajo
    // pendiente, es una mezcla de oportunidades imposibles, irrelevantes y no explotadas.
    //
    // CONSECUENCIA para la regla de oro: mientras el techo sea ESTRUCTURAL, "Remaining
    // decide el siguiente sprint" no aplica al splitting.  Para que vuelva a decidir hace
    // falta un techo de OPORTUNIDADES EXPLOTABLES (ventana libre que contenga al menos un
    // uso) -- un cambio de METRICA, no una correccion.  Es la leccion metodologica del
    // sprint: un techo mal elegido no da un numero equivocado, da un numero que no
    // responde a la pregunta que se le hace.
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
    const LaneOccupancy occ = lane_occupancy_of(p, la, bank);
    SpillTaxonomy t;
    for (const AbstractValue &v : p.values) {
        if (la.lane_of(v.value_id) != kSpilled) continue;
        const PosRange life{v.start, v.end};
        uint64_t max_free = 0; // maxima duracion libre sobre lanes admisibles.
        bool any_full = false;
        for (const Lane &l : bank.lanes) {
            if (!lane_admissible(v.req, l, vec_active)) continue;
            const uint64_t free_len = occ.free_time(l.id, life);
            if (free_len >= life.length()) { any_full = true; break; } // 100% libre.
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
