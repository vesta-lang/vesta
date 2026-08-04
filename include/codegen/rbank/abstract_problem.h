/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/abstract_problem.h
 * @brief Fase 0.5: el PROBLEMA abstracto de asignacion (SSA sintetico) + la
 *        abstraccion LiveRanges -> Interference.  SIN codigo real.
 *
 * Objetivo de la Fase 0.5: probar que el MODELO abstracto es FIEL antes de tocar
 * el compilador real.  Se trabaja sobre SSA SINTETICO (rangos de vida definidos a
 * mano o generados) para no depender del IR:
 *
 *     AbstractProblem (SSA sintetico: LiveRanges + ValueRequirements)
 *              |
 *        build_interference   (LiveRanges -> grafo de solapamiento)
 *              |
 *        ConstraintSet (aristas INTERFERE)  --> coloreado (coloring.h)
 *
 * INTERVAL GRAPH (P2): el grafo de interferencia de rangos SSA es un INTERVAL
 * GRAPH (cordal).  Su numero cromatico = el MAXIMO SOLAPAMIENTO en cualquier
 * punto (max clique).  @c max_overlap lo calcula: si el banco ofrece >=
 * max_overlap lanes de la clase, existe un coloreado SIN spill -> el coloreador
 * (coloring.h) DEBE encontrarlo.  Esa es la propiedad que valida el round-trip.
 *
 * i18n: produce DATOS (grafo/numeros), no diagnosticos -> sin catalogo.
 * Fase 0.5: ADITIVO, funciones puras, sin consumidores (solo el test/prototipo).
 */

#ifndef VESTA_CODEGEN_RBANK_ABSTRACT_PROBLEM_H
#define VESTA_CODEGEN_RBANK_ABSTRACT_PROBLEM_H

#include "analysis/facts/affinity_graph_facts.h"
#include "codegen/rbank/constraints.h"
#include "codegen/rbank/value_requirements.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @struct AbstractValue
 * @brief Un valor del SSA sintetico: su rango de vida [start,end] + sus Facts.
 *
 * SSA: un valor tiene UN solo punto de definicion (@c start) y vive hasta su
 * ultimo uso (@c end, inclusive, @c end>=start).  @c req son los Facts del valor
 * (clase/ancho/fixed_reg/...) -- los mismos ValueRequirements del modelo real.
 */
struct AbstractValue {
    uint32_t          value_id = 0;
    uint32_t          start    = 0; ///< posicion de definicion (SSA: unica).
    uint32_t          end      = 0; ///< ultima posicion de uso (>= start).
    ValueRequirements req;          ///< Facts del valor (clase/ancho/pin/...).
};

/**
 * @struct AbstractProblem
 * @brief Conjunto de valores del SSA sintetico (los LiveRanges de una "funcion") +
 *        el grafo de AFINIDAD (@c CopyGraphFacts, Fact de primer nivel que el
 *        coalescing consume).  En aislamiento el grafo es un input sintetico; en
 *        produccion sera @c snapshot.query<AffinityGraphFacts>() (alimentado por
 *        ssa_coalesce).  El problema TIRA del Fact -- no lo inventa.
 */
struct AbstractProblem {
    std::vector<AbstractValue> values;
    analysis::AffinityGraphFacts affinity; ///< afinidades (Fact; lo consume F3).
};

/**
 * @brief True si los rangos de vida de @p a y @p b SE SOLAPAN (viven a la vez).
 *
 * Rangos cerrados [start,end]: NO se solapan solo si uno acaba antes de que el
 * otro empiece.  Dos valores que se solapan interfieren (no pueden compartir la
 * misma lane fisica).
 */
inline bool ranges_overlap(const AbstractValue &a, const AbstractValue &b) noexcept {
    return !(a.end < b.start || b.end < a.start);
}

/**
 * @brief ABSTRACCION: LiveRanges -> Interference.  Emite una arista INTERFERE por
 *        cada par de valores cuyos rangos se solapan.  Funcion PURA y determinista
 *        (mismo problema -> mismo grafo) -> la abstraccion es reversible en el
 *        sentido del round-trip: re-abstraer da SIEMPRE lo mismo.
 *
 * O(n^2) deliberado (Fase 0.5 es un prototipo de validacion; el grafo eficiente
 * por barrido/bitsets es de Fase 2).
 */
inline ConstraintSet build_interference(const AbstractProblem &p) {
    ConstraintSet cs;
    const std::vector<AbstractValue> &v = p.values;
    for (size_t i = 0; i < v.size(); ++i)
        for (size_t j = i + 1; j < v.size(); ++j)
            if (ranges_overlap(v[i], v[j]))
                cs.interfere(v[i].value_id, v[j].value_id);
    return cs;
}

/**
 * @brief Extrae los @c ValueRequirements del problema (para el verificador).
 */
inline std::vector<ValueRequirements> collect_requirements(const AbstractProblem &p) {
    std::vector<ValueRequirements> out;
    out.reserve(p.values.size());
    for (const AbstractValue &av : p.values) out.push_back(av.req);
    return out;
}

/**
 * @brief Numero cromatico teorico de una CLASE: el maximo solapamiento (clique)
 *        entre los valores de esa clase.  En un interval graph = presion maxima.
 * @param cls  clase de recurso a medir (GP/FP_VECTOR/...).
 *
 * Barrido por eventos: +1 al empezar un rango, -1 al terminar.  El pico es el
 * numero de valores de la clase vivos a la vez = lanes MINIMAS necesarias para
 * colorear sin spill.  (Ignora fixed_reg/aliasing; es la cota del interval graph.)
 */
inline uint32_t max_overlap(const AbstractProblem &p, ResourceClass cls) {
    // Eventos (pos, delta): +1 en start, -1 en end+1 (rango cerrado).
    std::vector<std::pair<uint32_t, int>> ev;
    for (const AbstractValue &av : p.values) {
        if (av.req.cls != cls) continue;
        ev.push_back({av.start, +1});
        ev.push_back({av.end + 1, -1});
    }
    // Ordenar por posicion; a igual posicion, procesar las BAJAS (-1) antes que
    // las ALTAS (+1) para no contar como solapados dos rangos que solo se tocan
    // en el borde (a.end+1 == b.start).
    std::sort(ev.begin(), ev.end(),
              [](const std::pair<uint32_t, int> &x, const std::pair<uint32_t, int> &y) {
                  if (x.first != y.first) return x.first < y.first;
                  return x.second < y.second; // -1 antes que +1.
              });
    int cur = 0;
    uint32_t peak = 0;
    for (const std::pair<uint32_t, int> &e : ev) {
        cur += e.second;
        if (cur > static_cast<int>(peak)) peak = static_cast<uint32_t>(cur);
    }
    return peak;
}

/**
 * @brief True si dos conjuntos de interferencia tienen las MISMAS aristas
 *        (comparacion normalizada a<b, orden-independiente).  Usado por el
 *        round-trip para comprobar que re-abstraer coincide con la abstraccion
 *        de entrada (reversibilidad de los HECHOS).
 */
inline bool same_interference(const ConstraintSet &x, const ConstraintSet &y) {
    auto edges = [](const ConstraintSet &c) {
        std::vector<std::pair<uint32_t, uint32_t>> e;
        for (const Constraint &k : c.items)
            if (k.kind == ConstraintKind::INTERFERE) {
                uint32_t a = k.a, b = k.b;
                if (a > b) std::swap(a, b);
                e.push_back({a, b});
            }
        std::sort(e.begin(), e.end());
        e.erase(std::unique(e.begin(), e.end()), e.end());
        return e;
    };
    return edges(x) == edges(y);
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_ABSTRACT_PROBLEM_H
