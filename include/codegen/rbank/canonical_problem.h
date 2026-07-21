/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/canonical_problem.h
 * @brief Fase 1 (dominio): la IDENTIDAD del problema de asignacion.
 *
 * Define formalmente cuando dos problemas de asignacion son EL MISMO: conservan
 * la estructura de interferencia + clase + ancho + registros fijados, y eliminan
 * lo ARBITRARIO (value_ids, orden de la lista, offset absoluto de posiciones).
 * Es una relacion de EQUIVALENCIA ESTRUCTURAL; el @c canonical_hash es su testigo.
 *
 *     AbstractProblem  --canonicalize-->  CanonicalProblem { canon, hash, mapeos }
 *
 * Este fichero es el CONCEPTO DEL DOMINIO -- existe aunque nunca se reuse una
 * solucion.  La cache que se APOYA en el (solution_cache.h) es una optimizacion
 * separada.
 *
 * NORMALIZACION:
 *   - posiciones -> coordinate compression a coordenadas densas (0,1,2,...).  El
 *     solapamiento solo depende del ORDEN de los eventos y comp es monotona
 *     (a<b<c<d -> 0<1<2<3), asi que la interferencia NO cambia.  No se introduce
 *     informacion artificial.
 *   - value_id -> renombrado por la CLAVE (start,end,clase,ancho,pin), no por id.
 *   - orden de la lista -> irrelevante (se ordena por clave).
 * El hash es sobre la DESCRIPCION canonica, NO sobre el grafo de interferencia
 * (que es una consecuencia de la descripcion) -> evita reconstruirlo.
 *
 * i18n: produce DATOS (hash/estructura).  Fase 1: ADITIVO, funciones puras.
 */

#ifndef VESTA_CODEGEN_RBANK_CANONICAL_PROBLEM_H
#define VESTA_CODEGEN_RBANK_CANONICAL_PROBLEM_H

#include "codegen/rbank/abstract_problem.h"

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
 * Es un OBJETO CON SEMANTICA PROPIA (no un helper): representacion normalizada
 * (@c canon) + identidad (@c hash) + traduccion de ida (@c orig_to_canon) y de
 * vuelta (@c canon_to_orig).  @c canon tiene los value_ids renombrados a 0..n-1
 * (por orden de clave) y las posiciones comprimidas.  Los mapeos permiten llevar
 * una solucion expresada en ids CANONICOS de vuelta a los ids del problema real.
 */
struct CanonicalProblem {
    AbstractProblem                        canon;
    uint64_t                               hash = 0;
    std::vector<uint32_t>                  canon_to_orig; ///< canonical_id -> orig.
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

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_CANONICAL_PROBLEM_H
