/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/facts/affinity_graph_facts.h
 * @brief AffinityGraphFacts: el grafo de AFINIDAD entre valores (Fact de 1er nivel).
 *
 * A diferencia de LoopFacts/ProfileFacts (que analizan el PROGRAMA), este Fact
 * expresa una RELACION entre valores: "estos dos preferirian compartir color
 * fisico".  NO representa "hay un MOV"; representa la AFINIDAD -- un hecho del
 * problema de asignacion, independiente del algoritmo de coloreado.  La afinidad
 * es NO DIRIGIDA (@c a -- @c b, no @c a -> @c b).
 *
 * DATO PURO: solo responde "¿que valores tienen afinidad?".  NADA de heuristicas,
 * coste ni move-elimination -- esas son DECISIONES del Objective / solver, no del
 * Fact.  Tampoco @c coalesce_group / @c preferred_lane / @c move_chain.
 *
 * GENERALIZACION (la propiedad valiosa): la copia es solo la PRIMERA fuente de
 * afinidad.  Multiples fuentes producen EL MISMO Fact, y el consumidor no sabe de
 * donde vino cada arista -- por eso el nombre es "afinidad", no "copia":
 *
 *      QueryProducer<AffinityGraphFacts>
 *          edges += movs
 *          edges += phi lowering
 *          edges += two-address (dst=src1)
 *          edges += ISA register hints
 *          edges += vector pack/unpack
 *          edges += call ABI
 *      Objective / solver  ->  consume AffinityGraphFacts  (sin conocer el origen)
 *
 * En lugar de cuatro listas de "preferencias" repartidas por SSA-coalesce, el
 * lowering de PHI, el two-address y el ABI:  Programa -> Facts -> Objective.
 *
 * INTEGRACION FUTURA (query system): en el FunctionSnapshot sera una celda
 * LazyFact<AffinityGraphFacts> + QueryProducer<AffinityGraphFacts>, alimentada por
 * ssa_coalesce.cpp (produccion).  Hoy (Fase 3, aislamiento) lo lleva el
 * AbstractProblem como input sintetico.  El consumidor (coalescing) tira del Fact.
 */

#ifndef VESTA_ANALYSIS_FACTS_AFFINITY_GRAPH_FACTS_H
#define VESTA_ANALYSIS_FACTS_AFFINITY_GRAPH_FACTS_H

#include <cstdint>
#include <vector>

namespace analysis {

/**
 * @struct AffinityEdge
 * @brief Arista de afinidad NO DIRIGIDA: @c a y @c b preferirian compartir lane
 *        (p.ej. una copia entre ellos que se elimina si la comparten).
 * @c weight  importancia de mantenerlos juntos (la CONSUME el Objective/solver
 *        para priorizar cual romper primero; el Fact no decide nada con ella).
 */
struct AffinityEdge {
    uint32_t a      = 0;
    uint32_t b      = 0;
    float    weight = 1.0f;
};

/**
 * @struct AffinityGraphFacts
 * @brief Grafo de afinidades entre valores.  Solo el grafo -- dato puro.
 */
struct AffinityGraphFacts {
    std::vector<AffinityEdge> edges;
};

/** @enum AffinityCheck  @brief Comprobaciones de autocertificacion (DATOS). */
enum class AffinityCheck {
    SELF_EDGE,       ///< a == b (afinidad de un valor consigo mismo).
    NEGATIVE_WEIGHT, ///< peso < 0 (una afinidad nunca es "repulsion").
};

/// Hallazgo de la autocertificacion (DATO, no mensaje; i18n aguas arriba).
struct AffinityIssue {
    AffinityCheck check;
    uint32_t      value = 0;
};

/** @brief AUTOCERTIFICA el grafo: sin auto-aristas ni pesos negativos.
 *  (Deduplicacion de aristas a--b / b--a: cuando un consumidor la reclame.) */
inline std::vector<AffinityIssue> validate(const AffinityGraphFacts &g) {
    std::vector<AffinityIssue> out;
    for (const AffinityEdge &e : g.edges) {
        if (e.a == e.b)
            out.push_back({AffinityCheck::SELF_EDGE, e.a});
        if (e.weight < 0.0f)
            out.push_back({AffinityCheck::NEGATIVE_WEIGHT, e.a});
    }
    return out;
}

} // namespace analysis

#endif // VESTA_ANALYSIS_FACTS_AFFINITY_GRAPH_FACTS_H
