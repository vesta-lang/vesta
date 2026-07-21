/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/rbank/decision_policy.h
 * @brief Cuspide del modelo: el MOTOR DE DECISION (score / choose / explain).
 *
 * El @c DecisionEngine es el UNICO punto de seleccion del optimizador: recibe
 * un @c OptimizationContext y un conjunto de CANDIDATOS (validos segun
 * Constraints) y elige el de menor COSTE segun el Objective.  Lo decisivo del
 * diseno: la DECISION esta COMPLETAMENTE SEPARADA del algoritmo que la usa.
 *
 *     genera candidatos -> Constraints -> DecisionEngine -> resultado
 *
 * El allocator (o el scheduler, o el vectorizador) NO sabe COMO se decide.
 * Manana pueden coexistir @c WeightedObjectivePolicy, @c MLDecisionPolicy,
 * @c PGODecisionPolicy, @c EnergyDecisionPolicy... y el consumidor no cambia una
 * linea.  Eso es inusual en compiladores, donde estas decisiones suelen estar
 * repartidas entre muchos pases con heuristicas locales.
 *
 * TRES OPERACIONES (por eso es un ENGINE, no una simple politica):
 *   - @c score(candidate, ctx)   -> coste escalar (punto de enganche del Predictor).
 *   - @c choose(candidates, ctx) -> el mejor candidato VALIDO (argmin del coste;
 *     frontera de Pareto colapsada por los pesos; empate estable).
 *   - explicacion (@c DecisionExplanation) -> DATO i18n-ready: candidato elegido,
 *     conteos y los TOP contribuyentes al coste (para KB/ANAMNESIS + catalogo).
 *
 * PREDICTOR (P16): @c score es el enganche del aprendizaje.  Hoy heuristica
 * determinista; manana un modelo MLGO subclasea @c DecisionEngine con los mismos
 * candidatos + contexto como features, sin tocar a los consumidores.
 *
 * i18n: @c DecisionExplanation es DATO (enums + fracciones), NUNCA texto; el
 * consumidor lo mapea a un codigo @c VXNNNN del catalogo al mostrarlo.
 *
 * Fase 0: ADITIVO.  Interfaz + @c WeightedObjectivePolicy (argmin del objetivo
 * ponderado).  La generacion de candidatos y la enumeracion real de la frontera
 * de Pareto son de fases posteriores.
 */

#ifndef VESTA_JIT_RBANK_DECISION_POLICY_H
#define VESTA_JIT_RBANK_DECISION_POLICY_H

#include "jit/rbank/objective.h"
#include "jit/rbank/optimization_context.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace jit {
namespace rbank {

/**
 * @enum DecisionKind
 * @brief Que TIPO de decision representa un @c DecisionHandle.  Distintos
 *        consumidores deciden cosas distintas; el handle lleva su significado.
 */
enum class DecisionKind : uint8_t {
    GENERIC = 0,   ///< sin semantica especifica.
    LANE,          ///< eleccion de lane/registro (allocator).
    SPILL_SLOT,    ///< eleccion de slot de spill.
    ORDER,         ///< posicion en un orden (scheduler).
    VECTOR_WIDTH,  ///< ancho de vectorizacion.
    INSTRUCTION,   ///< eleccion de instruccion/patron (selector).
};

/**
 * @struct DecisionHandle
 * @brief Identificador tipado de un candidato.  Su @c value lo interpreta el
 *        consumidor segun @c kind (id de lane, indice de orden, ancho, ...).
 */
struct DecisionHandle {
    DecisionKind kind  = DecisionKind::GENERIC;
    uint32_t     value = 0;

    bool operator==(const DecisionHandle &o) const noexcept {
        return kind == o.kind && value == o.value;
    }
};

/** @brief Handle generico desde un valor (azucar). */
inline DecisionHandle handle(uint32_t value,
                             DecisionKind kind = DecisionKind::GENERIC) {
    return DecisionHandle{kind, value};
}

/**
 * @struct Candidate
 * @brief Un candidato de decision: su handle tipado + coste + validez.
 *
 * @c valid indica si paso el verificador de Constraints (validate_assignment);
 * los invalidos se descartan en @c choose.  El significado del candidato lo
 * interpreta el consumidor; el motor solo razona sobre coste.
 */
struct Candidate {
    DecisionHandle id;             ///< handle tipado del candidato.
    ObjectiveTerms terms;          ///< coste estimado.
    bool           valid = true;   ///< paso Constraints.
};

/**
 * @enum DominantTerm
 * @brief Dimension del Objective.  DATO para la explicacion i18n (no texto).
 */
enum class DominantTerm : uint8_t {
    NONE = 0, LATENCY, THROUGHPUT, CODE_SIZE, ENERGY,
    CACHE_PRESSURE, REGISTER_PRESSURE, SPILL, MOVE, CALLSAVE,
    DEPENDENCY, PORT_PRESSURE, SCHEDULER,
    COUNT
};

/**
 * @struct Contribution
 * @brief Aportacion de una dimension al coste elegido, con su fraccion.  Para
 *        una explicacion rica ("40% spill, 39% latency, 21% move").
 */
struct Contribution {
    DominantTerm term     = DominantTerm::NONE;
    double       weighted = 0.0; ///< peso*termino (coste absoluto aportado).
    double       fraction = 0.0; ///< fraccion del coste total [0,1].
};

/** @brief Numero de dimensiones del Objective. */
static constexpr size_t kObjectiveDims = static_cast<size_t>(DominantTerm::COUNT) - 1;

/** @brief Contribuciones ponderadas de TODAS las dimensiones (sin ordenar). */
inline std::array<Contribution, kObjectiveDims>
weighted_contributions(const ObjectiveTerms &t, const ObjectiveWeights &w) noexcept {
    std::array<Contribution, kObjectiveDims> c = {{
        {DominantTerm::LATENCY,           w.latency * t.latency, 0.0},
        {DominantTerm::THROUGHPUT,        w.throughput * t.throughput, 0.0},
        {DominantTerm::CODE_SIZE,         w.code_size * t.code_size, 0.0},
        {DominantTerm::ENERGY,            w.energy * t.energy, 0.0},
        {DominantTerm::CACHE_PRESSURE,    w.cache_pressure * t.cache_pressure, 0.0},
        {DominantTerm::REGISTER_PRESSURE, w.register_pressure * t.register_pressure, 0.0},
        {DominantTerm::SPILL,             w.spill * t.spill, 0.0},
        {DominantTerm::MOVE,              w.move * t.move, 0.0},
        {DominantTerm::CALLSAVE,          w.callsave * t.callsave, 0.0},
        {DominantTerm::DEPENDENCY,        w.dependency * t.dependency, 0.0},
        {DominantTerm::PORT_PRESSURE,     w.port_pressure * t.port_pressure, 0.0},
        {DominantTerm::SCHEDULER,         w.scheduler * t.scheduler, 0.0},
    }};
    double total = 0.0;
    for (const Contribution &e : c) total += e.weighted;
    if (total > 0.0)
        for (Contribution &e : c) e.fraction = e.weighted / total;
    return c;
}

/** @brief La dimension que mas aporta al coste (o NONE si todo es cero). */
inline DominantTerm dominant_term(const ObjectiveTerms &t,
                                  const ObjectiveWeights &w) noexcept {
    auto c = weighted_contributions(t, w);
    DominantTerm best = DominantTerm::NONE;
    double bestv = 0.0;
    for (const Contribution &e : c)
        if (e.weighted > bestv) { bestv = e.weighted; best = e.term; }
    return best;
}

/** @brief Numero de contribuyentes que guarda la explicacion (top-N). */
static constexpr size_t kTopContributors = 3;

/**
 * @struct DecisionExplanation
 * @brief Justificacion (DATO) de una decision, para ANAMNESIS + i18n.
 */
struct DecisionExplanation {
    DecisionHandle chosen;                ///< handle del candidato elegido.
    double         chosen_score          = 0.0;
    size_t         candidates_considered = 0;
    size_t         candidates_rejected   = 0; ///< invalidos descartados.
    bool           found                 = false;
    DominantTerm   dominant              = DominantTerm::NONE; ///< top-1.
    /// Los @c kTopContributors mayores contribuyentes al coste (desc), con fraccion.
    std::array<Contribution, kTopContributors> top{};
    size_t         top_count             = 0;
};

/** @brief Rellena @p out con los @p max mayores contribuyentes (desc). */
inline size_t fill_top_contributors(const ObjectiveTerms &t,
                                    const ObjectiveWeights &w,
                                    Contribution *out, size_t max) noexcept {
    auto c = weighted_contributions(t, w);
    std::sort(c.begin(), c.end(),
              [](const Contribution &a, const Contribution &b) {
                  return a.weighted > b.weighted;
              });
    size_t n = 0;
    for (const Contribution &e : c) {
        if (n >= max || e.weighted <= 0.0) break;
        out[n++] = e;
    }
    return n;
}

/**
 * @struct DecisionEngine
 * @brief Interfaz del motor de decision (score / choose / explain).  Subclasear
 *        para heuristicas alternativas, PGO, energia o MLGO.
 */
struct DecisionEngine {
    virtual ~DecisionEngine() = default;

    /** @brief Coste escalar del candidato en el contexto (punto Predictor). */
    virtual double score(const Candidate &c,
                         const OptimizationContext &ctx) const = 0;

    /**
     * @brief Elige el mejor candidato VALIDO (menor score; empate estable por
     *        orden de entrada).
     * @param out  si no es nullptr, se rellena con la explicacion (DATO).
     * @return puntero al candidato elegido, o @c nullptr si ninguno es valido.
     */
    virtual const Candidate *choose(const std::vector<Candidate> &candidates,
                                    const OptimizationContext &ctx,
                                    DecisionExplanation *out = nullptr) const = 0;
};

/**
 * @struct WeightedObjectivePolicy
 * @brief Motor determinista por defecto: score = suma ponderada del Objective;
 *        elige el valido de menor score (argmin), empate estable.  No es
 *        "greedy" (no hay decisiones locales iterativas): es un argmin global
 *        sobre los candidatos dados.
 */
struct WeightedObjectivePolicy : DecisionEngine {
    double score(const Candidate &c,
                 const OptimizationContext &ctx) const override {
        return objective_score(c.terms, ctx.weights());
    }

    const Candidate *choose(const std::vector<Candidate> &candidates,
                            const OptimizationContext &ctx,
                            DecisionExplanation *out = nullptr) const override {
        const Candidate *best = nullptr;
        double best_score = 0.0;
        size_t rejected = 0;
        for (const Candidate &c : candidates) {
            if (!c.valid) { ++rejected; continue; }
            const double s = score(c, ctx);
            if (!best || s < best_score) { best = &c; best_score = s; }
        }
        if (out) {
            out->candidates_considered = candidates.size();
            out->candidates_rejected   = rejected;
            out->found                 = (best != nullptr);
            if (best) {
                out->chosen       = best->id;
                out->chosen_score = best_score;
                out->dominant     = dominant_term(best->terms, ctx.weights());
                out->top_count    = fill_top_contributors(
                    best->terms, ctx.weights(), out->top.data(), kTopContributors);
            }
        }
        return best;
    }
};

} // namespace rbank
} // namespace jit

#endif // VESTA_JIT_RBANK_DECISION_POLICY_H
