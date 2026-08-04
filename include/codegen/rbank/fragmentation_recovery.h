/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/fragmentation_recovery.h
 * @brief FRAGMENTATION RECOVERY (splitting): produce un @c AssignmentPlan que devuelve a
 *        REGISTRO, durante los tramos en que hay una lane libre, los valores que el
 *        allocator derramo para TODA su vida.
 *
 *     Recovery Pass          recupera el spill ENTERO (necesita una lane libre siempre)
 *     Fragmentation Recovery recupera TRAMOS       (aprovecha huecos parciales)  <-- aqui
 *
 * Ataca la clase @c partially de la taxonomia: el 62,5% de los spills del corpus, cuyo
 * @c splitting_potential (el techo medido ANTES de implementar) es el 81,1% del area de
 * lane desperdiciada.  El KPI de la implementacion se compara contra ese techo
 * (Potential -> Recovered -> Remaining).
 *
 * RESPONSABILIDAD UNICA: produce un @c AssignmentPlan.  No construye segmentos, no conoce
 * el timeline, no emite movimientos, no toca la @c LaneAssignment.  La cadena es
 * estrictamente unidireccional, de modo que el ALGORITMO puede evolucionar (varias lanes,
 * otro cost model, region-based) sin tocar nada aguas abajo:
 *
 *     FragmentationRecovery -> AssignmentPlan -> TimelineBuilder -> AllocationTimeline
 *                                                                -> TransitionPlanner -> Rewrite
 *
 * ------------------------------------------------------------------------------------
 * CONDICIONES DE CORRECTITUD (por que un tramo es seguro)
 *
 * Un tramo [from,to) en registro exige DOS movimientos -- cargar en @c from y devolver a
 * memoria en @c to -- y el codigo solo es correcto si AMBOS se ejecutan, en ese orden, en
 * todo camino que atraviese el tramo.  De ahi las cuatro condiciones (todas VERIFICADAS
 * aqui, no supuestas):
 *
 *   1. RECTILINEO: el tramo cabe dentro de UN bloque, y @c to cae ESTRICTAMENTE dentro
 *      (nunca en su frontera).  Si @c to fuese el inicio del bloque siguiente, el
 *      "devolver a memoria" se ejecutaria tambien en los caminos que NO pasaron por el
 *      tramo -> escribiria basura en el slot.  (@c block_starts es el Fact que lo permite
 *      comprobar; hasta ahora nadie a este nivel necesitaba la estructura del CFG.)
 *   2. VIVO Y MATERIALIZADO: @c from > inicio del rango, para que exista un tramo previo
 *      en memoria -- si el tramo empezara en la definicion, el "cargar" leeria el slot
 *      antes de que nadie lo escribiera.
 *   3. VUELVE A LA BASE: @c to < fin del rango, para que exista un tramo posterior en
 *      memoria y el movimiento de vuelta se genere.  Sin el, un valor redefinido dentro
 *      del tramo dejaria el slot obsoleto.
 *   4. LANE ADMISIBLE Y LIBRE: @c lane_admissible (clase, ancho, pin, must-memory,
 *      callee-saved si cruza CALL) + libre en todo el tramo segun @c LaneOccupancy.  La
 *      admisibilidad se evalua con los Facts del valor ENTERO (conservador: un valor que
 *      cruza un CALL exige lane preservada aunque el tramo concreto no lo cruce).
 *
 * DOS CASOS QUE PARECEN UN AGUJERO Y NO LO SON, ambos cubiertos por la condicion 4 via
 * @c Residency::MEMORY (el hazard "debe-memoria" que ya traduce el backend_bridge):
 *
 *   - RAICES GC vivas a traves de un CALL: el GC las localiza por el stackmap, que
 *     describe SLOTS.  Tenerlas en registro durante el CALL las haria invisibles.  Son
 *     @c must_be_memory -> ninguna lane es admisible -> jamas se parten.
 *   - SALIDA ABNORMAL (throw): un @c throw a mitad de un tramo se lleva el valor del
 *     registro sin ejecutar el movimiento de vuelta, dejando el slot obsoleto.  Los
 *     valores live-in a un handler son @c force_spill (misma residencia MEMORY), asi que
 *     tampoco se parten; los demas no sobreviven al camino abnormal, luego no importa.
 *
 * Y un tercero, cubierto por la condicion 2: un valor live-in al header de un loop con
 * OSR vive en su slot en la posicion del header (el tramo empieza DESPUES), asi que la
 * entrada OSR lo restaura a memoria y la carga del tramo lee un valor coherente.
 *
 * Las condiciones 1-3 recortan cobertura a proposito.  Es la eleccion deliberada de este
 * incremento: primero correcto y medible, luego mas ambicioso -- lo que se pierde por cada
 * condicion se mide contra el techo y decide si merece un sprint (edge splitting, etc.).
 *
 * i18n: produce DATOS (plan + contadores), no diagnosticos -> sin catalogo.
 */

#ifndef VESTA_CODEGEN_RBANK_FRAGMENTATION_RECOVERY_H
#define VESTA_CODEGEN_RBANK_FRAGMENTATION_RECOVERY_H

#include "codegen/assignment_plan.h"
#include "codegen/rbank/abstract_problem.h"
#include "codegen/rbank/allowed_lanes.h"
#include "codegen/rbank/lane_occupancy.h"
#include "codegen/rbank/physical_bank.h"
#include "jit/interval.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @struct RecoveryCostModel
 * @brief Cuando MERECE LA PENA subir un tramo a registro.  Politica pura, separada del
 *        algoritmo: cambiar la economia no toca la busqueda de huecos.
 *
 * El beneficio NO es la duracion del hueco sino los USOS que contiene: un hueco de 400
 * posiciones sin un solo uso no ahorra nada, y uno de 4 con tres usos ahorra tres accesos
 * a memoria.  Por eso el modelo cuenta usos (que el backend ya calcula en
 * @c LiveInterval::uses) en vez de tomar la duracion como aproximacion.
 *
 *     beneficio = SUM( peso(uso) )  *  use_saving
 *     coste     = peso(from)*load_cost  +  peso(to)*store_cost
 *     recuperar si beneficio > coste
 *
 * @c weight_of es el gancho para la FRECUENCIA (un uso dentro de un bucle vale mas que uno
 * en codigo frio).  Hoy devuelve 1 -- no hay todavia un Fact de frecuencia a este nivel --
 * y con peso uniforme la formula se reduce a "numero de usos * use_saving > load+store".
 * Cuando exista, se enchufa AQUI sin tocar el algoritmo.
 *
 * Valores por defecto conservadores: con @c use_saving=1 y coste 1+1, hacen falta >=3 usos
 * dentro del tramo.  El motivo de ser conservador es concreto: en x86-64 el Rewrite PLIEGA
 * el slot como operando de memoria de la propia instruccion, asi que un uso derramado no
 * cuesta una instruccion extra -- el margen real es menor que en una ISA load-store.
 */
struct RecoveryCostModel {
    uint32_t use_saving = 1; ///< lo que ahorra CADA uso por vivir en registro.
    uint32_t load_cost  = 1; ///< movimiento de entrada (memoria -> registro).
    uint32_t store_cost = 1; ///< movimiento de salida  (registro -> memoria).

    /// Peso (frecuencia estimada) de una posicion.  Gancho: hoy uniforme.
    virtual uint32_t weight_of(uint32_t /*pos*/) const { return 1; }

    /**
     * @brief GANANCIA NETA (beneficio - coste) de subir a registro un tramo [from,to)
     *        con @p n_uses usos.  Positiva = compensa.
     *
     * Es UN SOLO criterio economico para las DOS preguntas del algoritmo -- cual es el
     * mejor hueco y si merece la pena -- en vez de elegir por un proxy (mas usos) y
     * aceptar por otro.  Importa en cuanto los pesos dejen de ser uniformes: entre "2
     * usos calientes" y "5 usos frios" gana el de mas ganancia, no el de mas usos.
     */
    int64_t net_gain(uint32_t n_uses, uint32_t from, uint32_t to) const {
        if (n_uses == 0) return 0;
        int64_t gain = 0;
        for (uint32_t i = 0; i < n_uses; ++i) gain += use_saving; // peso por uso: ver nota.
        gain *= static_cast<int64_t>(weight_of(from));
        const int64_t cost = static_cast<int64_t>(weight_of(from)) * load_cost +
                             static_cast<int64_t>(weight_of(to)) * store_cost;
        return gain - cost;
    }
    /* NOTA: el peso deberia evaluarse EN CADA USO (un uso dentro de un bucle vale mas),
     * no una vez para todo el tramo.  Con peso uniforme ambas formas coinciden, asi que
     * se deja la barata; cuando exista un Fact de frecuencia, el bucle de arriba pasa a
     * sumar weight_of(pos_del_uso) y el resto del algoritmo no se entera. */

    /// ¿Compensa el tramo?  Derivado de @c net_gain: un unico criterio, no dos.
    bool worth_it(uint32_t n_uses, uint32_t from, uint32_t to) const {
        return net_gain(n_uses, from, to) > 0;
    }

    virtual ~RecoveryCostModel() = default;
};

/**
 * @struct FragmentationStats
 * @brief Cuanto recupera de verdad la transformacion (el "Recovered" de la metodologia).
 *        Se compara con @c SpillTaxonomy::splitting_potential (el "Potential"); la
 *        diferencia es el "Remaining" que decide si hay siguiente sprint.
 */
struct FragmentationStats {
    uint32_t values_split   = 0; ///< valores derramados con al menos un tramo recuperado.
    uint32_t intervals      = 0; ///< tramos del plan.
    uint64_t recovered_area = 0; ///< area de lane devuelta a registro (posiciones).
    uint32_t uses_recovered = 0; ///< usos que pasan a leer de registro.
    uint32_t rejected_cost  = 0; ///< huecos descartados por el cost model.
    uint32_t rejected_shape = 0; ///< huecos descartados por las condiciones 1-3.

    /* PERFIL DE LA DECISION (3c.5): sumas para derivar las MEDIAS de lo aceptado frente a
     * lo rechazado.  Sin esto, mover @c use_saving / @c load_cost / @c store_cost seria
     * tuning a ciegas: se sabria que el numero sube, no POR QUE.  Con esto se puede
     * afirmar "el umbral optimo eran 2 usos" en vez de "movimos una constante".
     * Se guardan SUMAS, no histogramas: media = suma/contador, coste O(1) y agregable
     * entre funciones.  Lo aceptado ya es derivable (@c recovered_area / @c intervals). */
    int64_t  accepted_gain = 0; ///< suma de net_gain de los tramos aceptados (>0).
    uint64_t rejected_area = 0; ///< suma de la longitud de los tramos rechazados por coste.
    uint32_t rejected_uses = 0; ///< suma de usos de esos tramos rechazados.
    int64_t  rejected_gain = 0; ///< suma de su net_gain (<=0): cuanto les faltaba.

    void add(const FragmentationStats &o) noexcept {
        values_split += o.values_split;
        intervals += o.intervals;
        recovered_area += o.recovered_area;
        uses_recovered += o.uses_recovered;
        rejected_cost += o.rejected_cost;
        rejected_shape += o.rejected_shape;
        accepted_gain += o.accepted_gain;
        rejected_area += o.rejected_area;
        rejected_uses += o.rejected_uses;
        rejected_gain += o.rejected_gain;
    }
};

/**
 * @brief Bloque que CONTIENE la posicion @p pos (su indice), o 0 si no hay estructura.
 *        @p starts esta ordenado ascendente (@c IntervalResult::block_starts).
 */
inline size_t containing_block(const std::vector<uint32_t> &starts, uint32_t pos) noexcept {
    if (starts.empty()) return 0;
    const auto it = std::upper_bound(starts.begin(), starts.end(), pos);
    return static_cast<size_t>(it - starts.begin()) - (it == starts.begin() ? 0 : 1);
}

/**
 * @brief Produce el plan de splitting para los valores derramados en @p la.
 * @param p           el problema (vidas envolventes + Facts de cada valor).
 * @param la          la asignacion YA hecha (quien esta en registro y quien derramado).
 * @param bank        banco fisico del path (lanes, aliasing, preservacion).
 * @param vec_active  si el path usa lanes vectoriales (afecta a la admisibilidad).
 * @param ivs         rangos vivos, usos y fronteras de bloque -- los Facts que exigen las
 *                    condiciones de correctitud.
 * @param cost        politica economica.
 * @param stats       salida opcional de medicion.
 * @return el plan; VACIO si nada compensa (el pipeline no cambia el codigo emitido).
 */
inline AssignmentPlan build_fragmentation_plan(const AbstractProblem &p,
                                               const LaneAssignment &la,
                                               const PhysicalRegisterBank &bank,
                                               bool vec_active,
                                               const jit::IntervalResult &ivs,
                                               const RecoveryCostModel &cost,
                                               FragmentationStats *stats = nullptr) {
    AssignmentPlan plan;
    LaneOccupancy occ = lane_occupancy_of(p, la, bank);
    FragmentationStats st;
    std::vector<PosRange> windows;

    for (const AbstractValue &v : p.values) {
        if (la.lane_of(v.value_id) != kSpilled) continue;
        if (v.value_id >= ivs.intervals.size()) continue;
        const jit::LiveInterval &li = ivs.intervals[v.value_id];
        if (li.uses.empty()) continue; // sin usos no hay nada que ahorrar.

        // ¿Alguna lane puede alojarlo? (Facts del valor: clase/ancho/pin/must-memory/
        // cross-call).  Si ninguna, es un spill STRUCTURAL: ni mirar los huecos.
        std::vector<uint8_t> lanes;
        for (const Lane &l : bank.lanes)
            if (lane_admissible(v.req, l, vec_active)) lanes.push_back(l.id);
        if (lanes.empty()) continue;

        bool any_split = false;
        for (const jit::LiveRange &r : li.ranges) {
            if (r.to <= r.from + 1) continue; // rango demasiado corto para tener interior.

            // Condicion 1: recorrer el rango bloque a bloque -- un tramo NUNCA cruza una
            // frontera, para que carga y descarga compartan camino.
            size_t b = containing_block(ivs.block_starts, r.from);
            while (b < (ivs.block_starts.empty() ? 1u : ivs.block_starts.size())) {
                const uint32_t b_end = // fin (exclusive) del bloque b.
                    ivs.block_starts.empty()
                        ? ivs.max_pos
                        : (b + 1 < ivs.block_starts.size() ? ivs.block_starts[b + 1]
                                                           : ivs.max_pos);
                const uint32_t b_start =
                    ivs.block_starts.empty() ? 0u : ivs.block_starts[b];
                if (b_start >= r.to) break; // el bloque ya empieza tras el rango.

                // Condiciones 2 y 3: dentro del rango, con un tramo base ANTES y DESPUES.
                const uint32_t lo = std::max(b_start, r.from + 1u);   // from >= lo.
                const uint32_t hi = std::min(b_end, r.to);            // to  <  hi.
                ++b;
                /* Sin ningun uso en la zona no hay NADA que recuperar: no es un descarte,
                 * es ausencia de oportunidad.  Se filtra antes de contar para que
                 * @c rejected_shape mida lo que dice -- huecos que existian y no
                 * cupieron -- y no se infle con los tramos de codigo que ni tocan el
                 * valor (que son la inmensa mayoria). */
                bool any_use_here = false;
                for (uint32_t u : li.uses)
                    if (u >= lo && u < hi) { any_use_here = true; break; }
                if (!any_use_here) continue;

                // El tramo minimo ocupa [from, from+2) y exige from+2 < hi (condicion 3).
                if (hi < 3 || lo + 3 > hi) { ++st.rejected_shape; continue; }
                const PosRange zone{lo, hi - 1}; // cerrado: interior utilizable.

                // Mejor (lane, hueco) de la zona por GANANCIA NETA -- el mismo criterio
                // que decide despues si se acepta.  Determinista: a igualdad gana la lane
                // de menor id (orden del banco).
                uint8_t  best_lane = 0;
                uint32_t best_from = 0, best_to = 0, best_uses = 0;
                int64_t  best_gain = 0;
                bool     any_candidate = false;
                /* El MEJOR candidato en terminos absolutos, aunque su ganancia sea <=0:
                 * describe lo que se rechaza (¿por poco o por mucho?).  Solo perfilado. */
                uint32_t best_rej_from = 0, best_rej_to = 0, best_rej_uses = 0;
                bool     have_rej = false;
                for (uint8_t lane : lanes) {
                    occ.free_windows(lane, zone, windows);
                    for (const PosRange &w : windows) {
                        /* El tramo abarca [primer_uso, ultimo_uso + 2).
                         *
                         * El +2 NO es un margen: las posiciones van de dos en dos por
                         * instruccion (use_point = 2*gi, def_point = 2*gi+1), asi que
                         * `last` es el punto de USO de una instruccion y `last+1` es su
                         * punto de DEFINICION.  Cerrar en `last+2` incluye AMBOS -> si esa
                         * instruccion tambien REDEFINE el valor (op de dos direcciones), la
                         * escritura va al registro y el movimiento de vuelta -- que se
                         * emite en la frontera -- guarda el valor NUEVO.  Cerrar en `last+1`
                         * dejaria la definicion fuera del tramo y guardaria el viejo. */
                        uint32_t first = 0, last = 0, n = 0;
                        for (uint32_t u : li.uses) {
                            if (u < w.from) continue;
                            if (u + 1 > w.to) break; // el tramo no cabe en el hueco.
                            if (u + 3 > hi) break;   // to = u+2 debe quedar < hi (cond. 3).
                            if (n == 0) first = u;
                            last = u;
                            ++n;
                        }
                        if (n == 0) continue;
                        any_candidate = true;
                        const uint32_t end = last + 2; // fin (exclusive) del tramo.
                        const int64_t gain = cost.net_gain(n, first, end);
                        if (!have_rej || n > best_rej_uses) { // el "menos malo" (perfil).
                            have_rej = true;
                            best_rej_from = first;
                            best_rej_to = end;
                            best_rej_uses = n;
                        }
                        if (gain > best_gain) {
                            best_gain = gain;
                            best_uses = n; // solo para medir; NO entra en la decision.
                            best_lane = lane;
                            best_from = first;
                            best_to = end;
                        }
                    }
                }
                if (!any_candidate) { ++st.rejected_shape; continue; }
                if (best_gain <= 0) { // ninguno compensa: perfilar POR CUANTO no llego.
                    ++st.rejected_cost;
                    st.rejected_area += static_cast<uint64_t>(best_rej_to) - best_rej_from;
                    st.rejected_uses += best_rej_uses;
                    st.rejected_gain += best_gain;
                    continue;
                }

                plan.add(v.value_id, LinearPos{best_from}, LinearPos{best_to},
                         ValueLocation{ValueLocation::Register{best_lane}});
                // Comprometida: las decisiones siguientes ya ven la lane ocupada.
                occ.occupy(best_lane, PosRange{best_from, best_to - 1});
                ++st.intervals;
                st.recovered_area += static_cast<uint64_t>(best_to) - best_from;
                st.uses_recovered += best_uses;
                st.accepted_gain += best_gain;
                any_split = true;
            }
        }
        if (any_split) ++st.values_split;
    }

    if (stats) stats->add(st);
    return plan;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_FRAGMENTATION_RECOVERY_H
