/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/solution_cache.h
 * @brief Fase 1 (optimizacion): cache de coloreados indexada por la IDENTIDAD del
 *        problema (canonical_problem.h) x el banco.
 *
 * Responsabilidad SEPARADA de la canonicalizacion: la forma canonica es un
 * concepto del DOMINIO (existe aunque nunca reuses una solucion); esta cache es
 * una OPTIMIZACION que se apoya en ella.
 *
 *     CanonicalProblem.hash x bank_fingerprint  --hit-->  solucion canonica
 *                     |                                       |
 *                   miss                              traducir canon -> ids reales
 *                     |
 *               colorear + guardar
 *
 * CACHE KEY (P6/P9): la solucion depende del problema Y del BANCO (capabilities)
 * -> key = @c canonical_hash x @c bank_fingerprint.
 *
 * DIRECCION (no ahora): cuando exista mas de UN allocator (linear-scan vs PBQP vs
 * graph-coloring), la misma forma canonica puede dar soluciones DISTINTAS segun
 * la estrategia.  La identidad de la SOLUCION sera entonces
 * @c ProblemShape x @c Target x @c Policy: la key ganara un @c policy_id.  Hoy hay
 * una sola policy (color_linear_scan), asi que no se anade (regla: sin infra por
 * si acaso); entra con el solver de Fase 5.  El Objective entra por el mismo
 * sitio cuando el spill inteligente lo consuma.
 *
 * GATE: VESTA_NO_CANON (el consumidor puede colorear directo sin la cache).
 * i18n: produce DATOS (asignacion).  Fase 1: ADITIVO, sin consumidores de
 * produccion (solo el prototipo/test).
 */

#ifndef VESTA_CODEGEN_RBANK_SOLUTION_CACHE_H
#define VESTA_CODEGEN_RBANK_SOLUTION_CACHE_H

#include "codegen/rbank/canonical_problem.h"
#include "codegen/rbank/coloring.h"
#include "codegen/rbank/fnv.h"
#include "codegen/rbank/physical_bank.h"

#include <cstdint>
#include <unordered_map>

namespace codegen {
namespace rbank {

/**
 * @struct SolutionCache
 * @brief Cache de coloreados por (ProblemFingerprint x BankFingerprint).  Guarda
 *        la solucion en ids CANONICOS; @c solve la traduce a los ids del problema.
 *        La huella del PROBLEMA la da @c canonicalize; la del BANCO,
 *        @c bank.fingerprint() (la identidad es una pregunta del banco, no de la
 *        cache).  La cache solo las COMBINA en su key interna.
 */
struct SolutionCache {
    std::unordered_map<uint64_t, LaneAssignment> by_key; ///< key -> sol canonica.
    uint64_t hits = 0;
    uint64_t misses = 0;

    /**
     * @brief Colorea @p p reusando la cache.  Si la forma canonica (x banco) ya
     *        se resolvio, TRADUCE la solucion cacheada a los value_ids de @p p
     *        (cache HIT); si no, colorea la forma canonica, la guarda y la
     *        traduce (MISS).  El resultado es identico a colorear directo.
     */
    LaneAssignment solve(const AbstractProblem &p, const PhysicalRegisterBank &bank,
                         bool vec_active) {
        const CanonicalProblem cp = canonicalize(p);
        // Combinar las DOS huellas fuertes (problema x banco) en la key interna.
        const uint64_t key =
            fnv_mix(cp.fingerprint.value, bank.fingerprint(vec_active).value);

        auto it = by_key.find(key);
        LaneAssignment canon_sol;
        if (it != by_key.end()) {
            ++hits;
            canon_sol = it->second;
        } else {
            ++misses;
            canon_sol = color_linear_scan(cp.canon, bank, vec_active);
            by_key.emplace(key, canon_sol);
        }

        // Traducir la solucion canonica a los value_ids del problema original.
        LaneAssignment out;
        for (uint32_t cid = 0; cid < cp.canon_to_orig.size(); ++cid)
            out.lane[cp.canon_to_orig[cid]] = canon_sol.lane_of(cid);
        return out;
    }
};

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_SOLUTION_CACHE_H
