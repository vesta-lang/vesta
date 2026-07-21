/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/derived/profile_facts.h
 * @brief ProfileFacts: hecho DERIVADO del perfil, CENTRALIZADO en un productor.
 *
 * ===================== DOS TIPOS DE FACTS (arquitectura) =====================
 * El propio modelo revelo que hay dos clases de hechos:
 *   - TIPO A (inmediatos): salen DIRECTAMENTE del IR/CFG.  LoopFacts, DomFacts,
 *     LivenessFacts, AliasFacts.  Viven en @c analysis/facts/.
 *   - TIPO B (derivados): COMBINAN varios productores.  ProfileFacts, CostFacts,
 *     HotnessFacts, ExecutionFacts.  Viven en @c analysis/derived/ (aqui).
 * @c ProfileFacts es Tipo B: combina @c LoopFacts (Tipo A) con el perfil crudo.
 *
 * ===================== CENTRALIZACION DEL PROFILER ==========================
 * Hoy la info de perfil esta DISPERSA: contadores por-PC en
 * @c runtime::profile::g_profile, prob. de mispredict por-linea en
 * @c ir::g_branch_profile (if-conversion), y el profiler ligero
 * @c g_lite_branches.  @c ProfileFacts centraliza (como LoopFacts hizo con los
 * bucles): un solo productor de hechos de perfil por bloque/bucle que todos
 * consultan.
 *
 * ===================== FALTA UN NIVEL (dependencia revelada) =================
 * El trip-count NO sale directamente de unos contadores: sale de INTERPRETAR el
 * CFG (¿que rama sale?, ¿cual continua?, ¿varios exits?, switches, irreducibles,
 * bucles anidados).  Eso ya es un ANALISIS, no un fact trivial.  El camino
 * CORRECTO es por capas:
 *
 *     runtime  ->  BranchProfile  ->  CFGProfile   ->  LoopProfile  ->  ProfileFacts
 *   (g_profile)   (conteos/linea)   (bloques/aristas)  (trip-count)    (por bloque)
 *
 * v1 (ESTE fichero, aproximado y documentado): el trip-count se estima con la
 * heuristica "la rama COMUN de la cabecera continua, la RARA sale" (trip ~
 * max/min de los conteos de esa linea).  Ignora multiples exits, switches e
 * irreducibles -- suficiente para el caso comun (bucle contado/while), a
 * refinar con @c CFGProfile/@c LoopProfile cuando un consumidor exija precision.
 *
 * INPUT NEUTRAL: recibe un @c BranchProfile (conteos por LINEA), extraible del
 * profiler runtime (via pc->line) o del @c .vprof -> productor PURO y TESTEABLE
 * con perfiles sinteticos, sin acoplarse a la VM.  Sin perfil, @c weight = 0 ->
 * el @c OptimizationContext cae al estimador estatico por @c loop_depth.
 */

#ifndef VESTA_ANALYSIS_DERIVED_PROFILE_FACTS_H
#define VESTA_ANALYSIS_DERIVED_PROFILE_FACTS_H

#include "analysis/facts/loop_facts.h"
#include "ir/ssa_ir.h"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace analysis {

/**
 * @struct BranchProfile
 * @brief Perfil de branches por LINEA fuente (input neutral del productor).
 *
 * @c by_line[linea] = {taken, not_taken}.  Se rellena desde el profiler runtime
 * (contadores por-PC agregados por linea) o desde un @c .vprof.
 */
struct BranchProfile {
    std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> by_line;

    bool empty() const noexcept { return by_line.empty(); }
    void set(uint32_t line, uint64_t taken, uint64_t not_taken) {
        by_line[line] = {taken, not_taken};
    }
    /** @brief Devuelve los conteos de @p line (false si no hay). */
    bool counts(uint32_t line, uint64_t &taken, uint64_t &not_taken) const {
        auto it = by_line.find(line);
        if (it == by_line.end()) return false;
        taken = it->second.first;
        not_taken = it->second.second;
        return true;
    }
};

/**
 * @struct ProfileFacts
 * @brief Hechos de perfil por bloque y por bucle de una funcion (Tipo B).
 */
struct ProfileFacts {
    bool                has_profile = false; ///< hubo datos de perfil utiles.
    std::vector<double> block_weight;        ///< peso de ejecucion por bloque
                                             ///< (entry=1; 0 = desconocido).
    std::vector<double> trip_count;          ///< trip-count por bucle (0 = desconocido).

    double weight_of(ir::IrBlockId b) const noexcept {
        return b < block_weight.size() ? block_weight[b] : 0.0;
    }
    double trip_of(uint32_t loop) const noexcept {
        return loop < trip_count.size() ? trip_count[loop] : 0.0;
    }
};

/**
 * @brief Computa los @c ProfileFacts de una funcion (v1 aproximado, ver arriba).
 * @param fn     funcion SSA.
 * @param loops  hechos de bucles (cabeceras + anidamiento) -- productor Tipo A.
 * @param prof   perfil de branches por linea (puede estar vacio).
 * @return       hechos por bloque/bucle; @c has_profile=false si @p prof vacio.
 */
ProfileFacts compute_profile_facts(const ir::IrFunction &fn,
                                   const LoopFacts &loops,
                                   const BranchProfile &prof);

} // namespace analysis

#endif // VESTA_ANALYSIS_DERIVED_PROFILE_FACTS_H
