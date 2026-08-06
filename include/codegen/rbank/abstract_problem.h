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
#include <utility>
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
    /// Con quien le CONVIENE compartir lane, o -1.  Es una PREFERENCIA, no una
    /// exigencia: si se cumple, el movimiento que copiaria uno en otro
    /// desaparece; si no se puede, el codigo sigue siendo correcto y solo
    /// queda el movimiento.  Sale de la forma de dos operandos (`dst = src1 OP
    /// src2` -> a dst le conviene la lane de src1, porque el destino se pisa
    /// con el primer operando antes de operar).
    int32_t afinidad = -1;
    /// Los tramos REALES en los que el valor esta vivo, como ventana
    /// [tramos_off, tramos_off + tramos_n) del array plano del problema.
    /// @c tramos_n == 0 -> no se sabe y vale el envolvente [start,end], que es
    /// exacto para un valor sin huecos (el caso comun, y por eso no se guarda).
    ///
    /// Van en un array COMPARTIDO y no en un vector por valor: son decenas de
    /// miles por compilacion y el JIT paga su compilacion en el reloj del
    /// programa -- una reserva por valor se nota (~10% en los benches cortos).
    uint32_t tramos_off = 0;
    uint32_t tramos_n = 0;
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
    /// Array plano con los tramos de TODOS los valores; cada uno mira su
    /// ventana via @c tramos_off / @c tramos_n.  Una sola reserva por problema.
    std::vector<std::pair<uint32_t, uint32_t>> tramos;

    /// Cuantos tramos tiene @p v.  Un valor sin huecos no guarda ninguno, pero
    /// TIENE uno: su envolvente.  Contarlo como "sin datos" seria contagiar la
    /// imprecision al otro lado de la comparacion.
    uint32_t n_tramos(const AbstractValue &v) const noexcept {
        return v.tramos_n ? v.tramos_n : 1u;
    }

    /// El tramo @p i de @p v -- el envolvente si no guardo ninguno.
    std::pair<uint32_t, uint32_t> tramo(const AbstractValue &v,
                                        uint32_t i) const noexcept {
        if (v.tramos_n == 0) return {v.start, v.end};
        return tramos[v.tramos_off + i];
    }

    /**
     * @brief ¿Coinciden en el tiempo @p a y @p b -- estan vivos a la vez?
     *
     * Compara TRAMO a tramo.  Importa frente al envolvente porque un valor con
     * HUECOS no ocupa su registro en los huecos: tratarlo como si lo ocupara
     * inventa interferencias y derrama de mas.
     *
     * @param a Un valor.
     * @param b El otro.
     * @return true si comparten algun instante.
     */
    bool coinciden(const AbstractValue &a, const AbstractValue &b) const noexcept {
        // Descarte barato: sin solape de envolventes no hay nada que mirar, y
        // es el caso mayoritario.
        if (a.end < b.start || b.end < a.start) return false;
        if (a.tramos_n == 0 && b.tramos_n == 0) return true; // dos tramos unicos
        const uint32_t na = n_tramos(a), nb = n_tramos(b);
        for (uint32_t i = 0; i < na; ++i) {
            const std::pair<uint32_t, uint32_t> ra = tramo(a, i);
            for (uint32_t j = 0; j < nb; ++j) {
                const std::pair<uint32_t, uint32_t> rb = tramo(b, j);
                if (ra.first <= rb.second && rb.first <= ra.second) return true;
            }
        }
        return false;
    }
};

/**
 * @brief True si los ENVOLVENTES de @p a y @p b se solapan.
 *
 * Rangos cerrados [start,end]: no se solapan solo si uno acaba antes de que el
 * otro empiece.
 *
 * OJO: esto ignora los HUECOS.  Para saber si dos valores viven de verdad a la
 * vez -- que es lo que decide si pueden compartir lane -- hay que preguntarle
 * al problema (@c AbstractProblem::coinciden), que si los mira.  Esta version
 * sirve donde solo interesa el envolvente y como descarte barato.
 */
inline bool ranges_overlap(const AbstractValue &a, const AbstractValue &b) noexcept {
    return !(a.end < b.start || b.end < a.start);
}

/**
 * @brief ABSTRACCION: LiveRanges -> Interference.  Emite una arista INTERFERE por
 *        cada par de valores que viven A LA VEZ.  Funcion PURA y determinista
 *        (mismo problema -> mismo grafo) -> la abstraccion es reversible en el
 *        sentido del round-trip: re-abstraer da SIEMPRE lo mismo.
 *
 * Pregunta por los TRAMOS, no por el envolvente: si dijera que interfieren dos
 * valores que solo se cruzan en un hueco, el verificador daria por invalido un
 * reparto legitimo -- y el asignador y su juez tienen que medir con la misma
 * vara.
 *
 * O(n^2) deliberado (Fase 0.5 es un prototipo de validacion; el grafo eficiente
 * por barrido/bitsets es de Fase 2).
 */
inline ConstraintSet build_interference(const AbstractProblem &p) {
    ConstraintSet cs;
    const std::vector<AbstractValue> &v = p.values;
    for (size_t i = 0; i < v.size(); ++i)
        for (size_t j = i + 1; j < v.size(); ++j)
            if (p.coinciden(v[i], v[j]))
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
