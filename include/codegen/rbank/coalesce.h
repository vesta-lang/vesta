/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/coalesce.h
 * @brief Fase 3: COALESCING conservador sobre el problema abstracto.  Funde los
 *        valores AFINES que pueden compartir lane SIN aumentar la presion ->
 *        reciben la misma lane -> la copia se ELIMINA.
 *
 *     AbstractProblem { values, affinity }  --coalesce_conservative-->
 * AbstractProblem' (grupos fundidos)
 *
 * CRITERIO (correcto para interval graphs, NO Briggs):
 *   Se funde (a,b) si son AFINES Y:
 *     1. compatibilidad FISICA: misma ResourceClass, mismo ancho, pin
 * compatible (ambos sin pin, o el mismo).  Sin esto la fusion cambia la
 * semantica.
 *     2. NO interfieren (sus vidas no se solapan): dos valores en la misma lane
 * no pueden estar vivos a la vez.
 *     3. la fusion NO AUMENTA el @c max_overlap de la clase (no rompe la
 *        K-colorabilidad -> no reintroduce spill).
 *
 * POR QUE EL PUNTO 3 (matiz importante -- la seguridad es respecto al MODELO,
 * no a la teoria general).  El allocator NO trabaja con los intervalos
 * originales: trabaja con los @c AbstractValue que le pasamos, y hemos decidido
 * representar el grupo fundido por su ENVOLVENTE [min(start), max(end)] (un
 * solo intervalo, no multi-rango).  Por tanto la pregunta correcta NO es "¿la
 * union matematica aumenta el overlap?" sino "¿el modelo
 * AbstractValue-de-envolventes que voy a pasar al allocator aumenta la
 * presion?".  Eso es EXACTAMENTE lo que mide el punto 3
 * (@c hull_overlap sobre las envolventes).  La propiedad "fundir
 * no-interferentes preserva max_overlap" es cierta para la UNION REAL, pero el
 * ENVOLVENTE puede cubrir un HUECO donde vivia OTRO valor -> subir el cromatico
 * DEL MODELO.  Ejemplo: A=[0,3] y B=[10,12] (afines, no interfieren) con
 * C=[5,8] entre medias: el envolvente [0,12] pasa a interferir con C.  Para
 * afinidades de la MISMA VIDA LOGICA (a muere, b nace justo despues -- el caso
 * normal de COPY/PHI) el envolvente = union real, no hay hueco, y el punto 3 lo
 * acepta; los "con hueco" los RECHAZA. Asi la representacion simple
 * (envolvente) sigue siendo segura RESPECTO AL MODELO, sin necesidad de valores
 * multi-rango.
 *
 * IMPORTANTE: el punto (3) NO es una propiedad del coalescing -- es el PRECIO
 * de elegir representar el grupo por su ENVOLVENTE.  Dos caminos: Opcion A
 * (esta): grupo = envolvente + protegerlo con el check de max_overlap.
 *     Correcto; O(n^3) en el prototipo.
 *   Opcion B (diferida): grupo = CONJUNTO de intervalos (RangeSet) en vez de
 *     envolvente.  Desaparecen los huecos -> el punto (3) NO hace falta, ni el
 *     O(n^3); pero el modelo del allocator pasa de Range a RangeSet (demasiado
 * para Fase 3).  Se elige A para el prototipo; cuando el allocator real
 * justifique multi-rango, B elimina el check por completo.
 *
 * El grafo es el Fact @c AffinityGraphFacts; el coalescing lo CONSUME.  En
 * produccion vendra de @c snapshot.query<AffinityGraphFacts>() (ssa_coalesce);
 * en aislamiento lo lleva el AbstractProblem.  Forma uniforme con
 * CanonicalProblem: transformacion pura AbstractProblem -> AbstractProblem' +
 * mapeos + metrica (empieza a emerger la familia AbstractProblem -> Canonical
 * -> Coalesced -> Colored).
 *
 * i18n: produce DATOS.  Fase 3: ADITIVO, funcion pura, sin consumidores de
 * produccion (solo el prototipo/test).  O(n^3) deliberado (prototipo).
 */

#ifndef VESTA_CODEGEN_RBANK_COALESCE_H
#define VESTA_CODEGEN_RBANK_COALESCE_H

#include "codegen/rbank/abstract_problem.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @struct CoalesceResult
 * @brief Problema con los grupos afines fundidos + cuantas afinidades se
 *        realizaron (copias eliminadas) + el mapeo de cada value_id original a
 * su representante.
 */
struct CoalesceResult {
    AbstractProblem problem; ///< grupos fundidos.
    uint32_t copies_eliminated = 0;
    std::unordered_map<uint32_t, uint32_t>
        rep; ///< orig value_id -> representante.
};

/**
 * @brief Pico de solapamiento (max_overlap) de la clase @p cls sobre una lista
 * de intervalos-envolvente (start,end).  Barrido de eventos: -1 antes que +1 a
 *        igual posicion (dos rangos que solo se tocan en el borde no solapan).
 */
inline uint32_t
hull_overlap(const std::vector<std::pair<uint32_t, uint32_t>> &iv) {
    std::vector<std::pair<uint32_t, int>> ev;
    ev.reserve(iv.size() * 2);
    for (const auto &r : iv) {
        ev.push_back({r.first, +1});
        ev.push_back({r.second + 1, -1});
    }
    std::sort(ev.begin(), ev.end(), [](const auto &x, const auto &y) {
        if (x.first != y.first) return x.first < y.first;
        return x.second < y.second;
    });
    int cur = 0;
    uint32_t peak = 0;
    for (const auto &e : ev) {
        cur += e.second;
        if (cur > static_cast<int>(peak)) peak = static_cast<uint32_t>(cur);
    }
    return peak;
}

/**
 * @brief Coalescing CONSERVADOR (ver criterio en la cabecera del fichero).
 *        Funcion pura: AbstractProblem -> AbstractProblem' con los grupos
 * fundidos.
 */
inline CoalesceResult coalesce_conservative(const AbstractProblem &p) {
    CoalesceResult out;
    const size_t n = p.values.size();

    std::unordered_map<uint32_t, size_t> idx; // value_id -> indice.
    for (size_t i = 0; i < n; ++i)
        idx[p.values[i].value_id] = i;

    // grp[i] = grupo del valor i (inicialmente cada valor su propio grupo).
    std::vector<int> grp(n);
    for (size_t i = 0; i < n; ++i)
        grp[i] = static_cast<int>(i);

    // Hulls (envolventes) de una clase, con la fusion tentativa gy->gx tratada
    // como UN solo grupo.  (gx=-1,gy=-2 = "sin fusion": no matchean ningun
    // grupo real.)
    auto class_hulls = [&](ResourceClass cls, int gx, int gy) {
        std::unordered_map<int, std::pair<uint32_t, uint32_t>> seen;
        for (size_t i = 0; i < n; ++i) {
            if (p.values[i].req.cls != cls) continue;
            int g = grp[i];
            if (g == gy) g = gx; // fusion tentativa.
            auto it = seen.find(g);
            if (it == seen.end())
                seen[g] = {p.values[i].start, p.values[i].end};
            else {
                it->second.first =
                    std::min(it->second.first, p.values[i].start);
                it->second.second =
                    std::max(it->second.second, p.values[i].end);
            }
        }
        std::vector<std::pair<uint32_t, uint32_t>> iv;
        iv.reserve(seen.size());
        for (const auto &kv : seen)
            iv.push_back(kv.second);
        return iv;
    };
    auto groups_interfere = [&](int g1, int g2) {
        for (size_t i = 0; i < n; ++i) {
            if (grp[i] != g1) continue;
            for (size_t j = 0; j < n; ++j)
                if (grp[j] == g2 && ranges_overlap(p.values[i], p.values[j]))
                    return true;
        }
        return false;
    };
    auto group_fixed = [&](int g) -> int16_t {
        for (size_t i = 0; i < n; ++i)
            if (grp[i] == g && p.values[i].req.fixed_reg >= 0)
                return p.values[i].req.fixed_reg;
        return -1;
    };

    for (const analysis::AffinityEdge &e : p.affinity.edges) {
        auto ia = idx.find(e.a), ib = idx.find(e.b);
        if (ia == idx.end() || ib == idx.end()) continue;
        int ga = grp[ia->second], gb = grp[ib->second];
        if (ga == gb) continue; // ya en el mismo grupo.
        const ValueRequirements &ra = p.values[ia->second].req;
        const ValueRequirements &rb = p.values[ib->second].req;
        // (1) compatibilidad fisica.
        if (ra.cls != rb.cls || ra.width != rb.width) continue;
        const int16_t fa = group_fixed(ga), fb = group_fixed(gb);
        if (fa >= 0 && fb >= 0 && fa != fb) continue;
        // (2) no interfieren (member-wise).
        if (groups_interfere(ga, gb)) continue;
        // (3) la fusion (por envolvente) NO aumenta el max_overlap de la clase.
        const uint32_t before =
            hull_overlap(class_hulls(ra.cls, -1, -2)); // sin fusion.
        const uint32_t after =
            hull_overlap(class_hulls(ra.cls, ga, gb)); // con fusion.
        if (after > before)
            continue; // rechaza (el envolvente crea interferencia).

        // Fundir gb en ga.
        for (size_t i = 0; i < n; ++i)
            if (grp[i] == gb) grp[i] = ga;
        ++out.copies_eliminated;
    }

    // Reconstruir: un AbstractValue por grupo (envolvente + clase/ancho + pin).
    std::unordered_map<int, size_t> group_repr;
    for (size_t i = 0; i < n; ++i) {
        const int g = grp[i];
        const AbstractValue &v = p.values[i];
        auto it = group_repr.find(g);
        if (it == group_repr.end()) {
            group_repr[g] = out.problem.values.size();
            out.problem.values.push_back(v); // hereda clase/ancho/pin.
        } else {
            AbstractValue &av = out.problem.values[it->second];
            av.start = std::min(av.start, v.start);
            av.end = std::max(av.end, v.end);
            if (v.value_id < av.value_id) {
                av.value_id = v.value_id;
                av.req.value_id = v.value_id;
            }
            if (av.req.fixed_reg < 0 && v.req.fixed_reg >= 0)
                av.req.fixed_reg = v.req.fixed_reg;
        }
    }
    for (size_t i = 0; i < n; ++i)
        out.rep[p.values[i].value_id] =
            out.problem.values[group_repr[grp[i]]].value_id;

    return out;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_COALESCE_H
