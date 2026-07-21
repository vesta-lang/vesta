/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/canonical.h
 * @brief Fase 1: CANONICALIZACION del problema de asignacion + cache de soluciones.
 *
 * Idea (P6): si dos CFG distintos producen los MISMOS Facts de asignacion
 * (LiveRanges + interferencia + clase/ancho + pin), el allocator recibe el MISMO
 * problema.  Canonicalizar borra lo ARBITRARIO (etiquetas de value_id, offset
 * absoluto de las posiciones, orden de la lista) y deja una FORMA CANONICA cuyo
 * @c canonical_hash es invariante bajo esas transformaciones.  Entonces el
 * coloreado se computa UNA vez por forma y se REUSA:
 *
 *     AbstractProblem  --canonicalize-->  CanonicalProblem { canon, hash, mapeos }
 *                                              |
 *                        cache[hash x bank]  --hit-->  solucion canonica
 *                                              |          |
 *                                            miss    traducir canon -> ids del
 *                                              |      problema original
 *                                        colorear + guardar
 *
 * QUE ES ARBITRARIO (se normaliza):
 *   - value_id: etiquetas.  Se renombran por orden de la CLAVE (no del id).
 *   - posiciones absolutas: solo importa el ORDEN de los eventos -> coordinate
 *     compression a coordenadas densas (0,1,2,...).  Preserva el solapamiento
 *     (comp es monotona) -> misma interferencia.
 *   - orden de la lista de valores.
 * QUE ES SIGNIFICATIVO (entra en el hash): la estructura de solapamiento + clase
 * + ancho + pin de cada valor.
 *
 * CACHE KEY (P6/P9): la solucion depende del BANCO (capabilities) -> la key es
 * @c canonical_hash x @c bank_fingerprint.  El Objective entrara en la key cuando
 * el spill inteligente (Fase 5) lo consuma; el coloreo naive de Fase 2 no depende
 * de el.
 *
 * GATE: VESTA_NO_CANON (el consumidor puede saltarse la cache y colorear directo).
 * i18n: produce DATOS (hash/asignacion).  Fase 1: ADITIVO, sin consumidores de
 * produccion (solo el prototipo/test).
 */

#ifndef VESTA_CODEGEN_RBANK_CANONICAL_H
#define VESTA_CODEGEN_RBANK_CANONICAL_H

#include "codegen/rbank/abstract_problem.h"
#include "codegen/rbank/coloring.h"
#include "codegen/rbank/physical_bank.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace codegen {
namespace rbank {

/** @brief Semilla y primo FNV-1a de 64 bits. */
static constexpr uint64_t kFnvOffset = 1469598103934665603ull;
static constexpr uint64_t kFnvPrime  = 1099511628211ull;

/** @brief Mezcla un entero de 64 bits en un acumulador FNV-1a. */
inline uint64_t fnv_mix(uint64_t h, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= (v & 0xff);
        h *= kFnvPrime;
        v >>= 8;
    }
    return h;
}

/**
 * @struct CanonicalProblem
 * @brief Forma canonica de un @c AbstractProblem + hash + mapeos de traduccion.
 *
 * @c canon tiene los value_ids renombrados a 0..n-1 (por orden de clave) y las
 * posiciones comprimidas.  @c canon_to_orig[c] = value_id original del canonico
 * @c c; @c orig_to_canon[v] = canonico del original @c v.  Sirven para traducir
 * una solucion cacheada (en ids canonicos) de vuelta a los ids del problema real.
 */
struct CanonicalProblem {
    AbstractProblem                     canon;
    uint64_t                            hash = 0;
    std::vector<uint32_t>               canon_to_orig; ///< indexado por canonical_id.
    std::unordered_map<uint32_t, uint32_t> orig_to_canon;
};

/**
 * @brief Canonicaliza @p p: comprime posiciones + renombra value_ids por CLAVE +
 *        computa el hash invariante.  Funcion pura.
 */
inline CanonicalProblem canonicalize(const AbstractProblem &p) {
    CanonicalProblem cp;

    // 1) Coordinate compression: puntos de evento -> indices densos.
    std::vector<uint32_t> pts;
    pts.reserve(p.values.size() * 2);
    for (const AbstractValue &v : p.values) {
        pts.push_back(v.start);
        pts.push_back(v.end);
    }
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    auto compress = [&](uint32_t x) -> uint32_t {
        return static_cast<uint32_t>(
            std::lower_bound(pts.begin(), pts.end(), x) - pts.begin());
    };

    // 2) Clave canonica por valor (SIN el value_id: los ids son arbitrarios).
    struct Keyed {
        uint32_t          orig_id;
        uint32_t          s, e;     // posiciones comprimidas.
        ValueRequirements req;
    };
    std::vector<Keyed> ks;
    ks.reserve(p.values.size());
    for (const AbstractValue &v : p.values)
        ks.push_back({v.value_id, compress(v.start), compress(v.end), v.req});

    // Orden canonico por la clave (start, end, clase, ancho, pin).  El orig_id es
    // solo el DESEMPATE final (determinista); no entra en el hash.
    std::sort(ks.begin(), ks.end(), [](const Keyed &a, const Keyed &b) {
        if (a.s != b.s) return a.s < b.s;
        if (a.e != b.e) return a.e < b.e;
        if (a.req.cls != b.req.cls) return a.req.cls < b.req.cls;
        if (a.req.width != b.req.width) return a.req.width < b.req.width;
        if (a.req.fixed_reg != b.req.fixed_reg) return a.req.fixed_reg < b.req.fixed_reg;
        return a.orig_id < b.orig_id;
    });

    // 3) Construir el problema canonico + mapeos + hash (sobre las CLAVES, sin id).
    uint64_t h = kFnvOffset;
    cp.canon_to_orig.reserve(ks.size());
    for (uint32_t cid = 0; cid < ks.size(); ++cid) {
        const Keyed &k = ks[cid];
        AbstractValue av;
        av.value_id = cid;
        av.start = k.s;
        av.end = k.e;
        av.req = k.req;
        av.req.value_id = cid;
        cp.canon.values.push_back(av);
        cp.canon_to_orig.push_back(k.orig_id);
        cp.orig_to_canon[k.orig_id] = cid;

        h = fnv_mix(h, k.s);
        h = fnv_mix(h, k.e);
        h = fnv_mix(h, static_cast<uint64_t>(k.req.cls));
        h = fnv_mix(h, static_cast<uint64_t>(k.req.width));
        h = fnv_mix(h, static_cast<uint64_t>(static_cast<int64_t>(k.req.fixed_reg)));
    }
    cp.hash = h;
    return cp;
}

/** @brief Hash canonico de @p p (conveniencia; = canonicalize(p).hash). */
inline uint64_t canonical_hash(const AbstractProblem &p) {
    return canonicalize(p).hash;
}

/**
 * @brief Huella del banco (capabilities) para la cache key: dos bancos que
 *        colorearian distinto deben dar huellas distintas.  Incluye el nombre
 *        (ABI) + las lanes asignables por clase relevante.
 */
inline uint64_t bank_fingerprint(const PhysicalRegisterBank &bank,
                                 bool vec_active) {
    uint64_t h = kFnvOffset;
    for (char c : bank.name) h = fnv_mix(h, static_cast<uint64_t>(c));
    for (ResourceClass cls : {ResourceClass::GP, ResourceClass::FP_VECTOR,
                              ResourceClass::MASK, ResourceClass::PREDICATE})
        h = fnv_mix(h, bank.allocatable_count(cls, vec_active));
    h = fnv_mix(h, vec_active ? 1u : 0u);
    return h;
}

/**
 * @struct SolutionCache
 * @brief Cache de coloreados por (canonical_hash x bank_fingerprint).  Guarda la
 *        solucion en ids CANONICOS; @c solve la traduce a los ids del problema.
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
        const uint64_t key = fnv_mix(cp.hash, bank_fingerprint(bank, vec_active));

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

#endif // VESTA_CODEGEN_RBANK_CANONICAL_H
