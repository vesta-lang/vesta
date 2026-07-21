/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/optimization_context.h
 * @brief Nivel 4 del modelo: el CONTEXTO que recibe la DecisionPolicy.
 *
 * @c OptimizationContext es la FIRMA ESTABLE que la @c DecisionPolicy (y
 * cualquier consumidor: allocator, scheduler, vectorizador) recibe.  En vez de
 * pasar Facts sueltos (y tener que cambiar la firma cada vez que aparece una
 * senal nueva), se pasa ESTE objeto.  Un MLGO futuro podra leer cache_pressure,
 * branch_entropy, energia, temperatura o presupuesto de compilacion SIN tocar
 * la firma de nadie.
 *
 *     PhysicalRegisterBank -> ValueRequirements -> Constraints -> Objective
 *                                                                     |
 *                                                        OptimizationContext  <-- aqui
 *                                                                     |
 *                                                              DecisionPolicy
 *
 * PRINCIPIO CLAVE (aprendizaje de diseno): el contexto son ACCESSORS/PUNTEROS a
 * las piezas, NO copias EAGER.  No se paga computar un analisis (Region,
 * CandidateSpace, History) si el consumidor no lo toca.  En Fase 0.25 esos
 * punteros seran accessors lazy al AnalysisManager (la capa de Facts ya existe:
 * liveness/points-to/escape/effects); aqui se dejan los HUECOS documentados.
 *
 * PUNTO DE CONVERGENCIA (Fase 0.25): el contexto es donde se juntan las DOS
 * fuentes de conocimiento.  @c execution_weight(r) trae los Facts del PROGRAMA
 * (loop_depth, hotness medida); @c hw_cost trae los Facts del HARDWARE (Tipo C:
 * coste real de reload/store).  @c spill_terms_for(r) los combina en los
 * @c ObjectiveTerms que la DecisionPolicy puntua:
 *
 *     Facts del programa  +  Facts del hardware  ->  Terms  ->  Decision
 *
 * @c Budget es distinto del @c Objective: el objetivo son PREFERENCIAS (que es
 * mejor); el presupuesto son TOPES DUROS (compilacion/memoria/energia) que no se
 * pueden exceder (mas cerca de un Constraint).  Dos objetivos identicos con
 * presupuestos distintos producen decisiones distintas.
 *
 * Fase 0: ADITIVO.  Bundle de lo que YA existe (bank=Capabilities+fisico,
 * constraints, objective, budget, flag de reduccion) + los huecos.  Sin
 * consumidores salvo el test.
 */

#ifndef VESTA_CODEGEN_RBANK_OPTIMIZATION_CONTEXT_H
#define VESTA_CODEGEN_RBANK_OPTIMIZATION_CONTEXT_H

#include "codegen/rbank/constraints.h"
#include "codegen/rbank/objective.h"
#include "codegen/rbank/physical_bank.h"

#include <cstdint>

namespace codegen {
namespace rbank {

/**
 * @struct Budget
 * @brief Topes DUROS de recursos de compilacion (no preferencias).  Un valor 0
 *        significa "sin limite" en esa dimension.
 */
struct Budget {
    uint64_t compile_ns   = 0; ///< tiempo de compilacion (ns).  0 = ilimitado.
    uint64_t memory_bytes = 0; ///< memoria de trabajo (bytes).  0 = ilimitado.
    double   energy       = 0.0;///< presupuesto de energia.     0 = ilimitado.

    bool unlimited_compile() const noexcept { return compile_ns == 0; }
    bool unlimited_memory()  const noexcept { return memory_bytes == 0; }
    bool unlimited_energy()  const noexcept { return energy == 0.0; }

    /** @brief True si @p spent_ns supera el tope de compilacion. */
    bool compile_exceeded(uint64_t spent_ns) const noexcept {
        return !unlimited_compile() && spent_ns > compile_ns;
    }
    /** @brief True si @p used_bytes supera el tope de memoria. */
    bool memory_exceeded(uint64_t used_bytes) const noexcept {
        return !unlimited_memory() && used_bytes > memory_bytes;
    }
    /** @brief True si @p used_energy supera el tope de energia. */
    bool energy_exceeded(double used_energy) const noexcept {
        return !unlimited_energy() && used_energy > energy;
    }
};

/**
 * @struct OptimizationContext
 * @brief Firma estable que recibe la DecisionPolicy: punteros a las piezas +
 *        objetivo/presupuesto por valor.
 *
 * Los punteros son NULABLES: un contexto de Fase 0 puede no tener aun Facts,
 * Profile, Region, CandidateSpace o History cableados.  Las accessors permiten
 * consultar disponibilidad sin desreferenciar a ciegas.
 */
struct OptimizationContext {
    // --- Piezas presentes en Fase 0 (punteros = sin copia eager) ---
    const PhysicalRegisterBank *bank        = nullptr; ///< Capabilities + fisico.
    const ConstraintSet        *constraints = nullptr; ///< feasibilidad dura.
    ObjectiveWeights            objective;              ///< preferencias (tunable).
    Budget                      budget;                 ///< topes duros.
    bool                        vec_reduction_active = false; ///< path VEC_ACC activo.

    // --- Fact del HARDWARE (Tipo C), cableado en Fase 0.25 ---
    // La SpillCostCard es el Fact Tipo C YA REDUCIDO (3 ciclos ISA-neutrales:
    // reload/store/move) que produce el CostAdapter desde MachineCostFacts.  Se
    // guarda POR VALOR (no es un analisis caro: es un resumen barato, como
    // objective/budget); esto NO viola el principio "sin copias eager" (ese
    // aplica a analisis como Region/CandidateSpace/History).  Default from_hw=
    // false -> fallback generico (nadie pago un probe).  El contexto es asi el
    // PUNTO DE CONVERGENCIA de las dos fuentes: programa (execution_weight) +
    // hardware (hw_cost) -> terms -> decision.
    SpillCostCard               hw_cost;               ///< coste HW (Tipo C reducido).

    // --- Huecos para Fase 0.25+ (accessors lazy al AnalysisManager) ---
    // Se anaden como punteros a los analisis cuando existan, NUNCA copiados:
    //   * Facts        : liveness / loop-info / points-to / escape (capa analysis).
    //   * Profile      : g_profile / .vprof (hotness real, trip-count).
    //   * Region       : hot/cold, loop regions (OptimizationRegion).
    //   * CandidateSpace: espacio de candidatos persistente y cacheado.
    //   * History      : DecisionTrace en ANAMNESIS (para debug + MLGO).
    // Anadirlos aqui NO cambia la firma de la DecisionPolicy (esa es la idea).

    // --- Accessors ---
    bool has_bank() const noexcept { return bank != nullptr; }
    bool has_constraints() const noexcept { return constraints != nullptr; }
    bool has_hw_cost() const noexcept { return hw_cost.from_hw; }
    const PhysicalRegisterBank &get_bank() const noexcept { return *bank; }
    const ConstraintSet &get_constraints() const noexcept { return *constraints; }
    const ObjectiveWeights &weights() const noexcept { return objective; }
    const SpillCostCard &hw() const noexcept { return hw_cost; }

    /** @brief Score de unos terminos con los pesos de este contexto. */
    double score(const ObjectiveTerms &t) const noexcept {
        return objective_score(t, objective);
    }

    /**
     * @brief Peso de ejecucion de un valor -- el CONTEXTO lo suministra (Facts
     *        del PROGRAMA).
     *
     * Fase 0: devuelve el fallback estatico @c static_execution_weight(loop_depth).
     * Fase 0.25: cuando el Profile (.vprof / g_profile) este cableado, devolvera
     * la frecuencia MEDIDA del bloque de la definicion; asi el modelo no queda
     * atado a la heuristica @c 10^depth.
     */
    double execution_weight(const ValueRequirements &r) const noexcept {
        // Preferir la frecuencia MEDIDA (ProfileFacts) si esta presente; si no
        // (0 = sin perfil), caer al estimador estatico por profundidad de loop.
        return r.execution_weight > 0.0 ? r.execution_weight
                                        : static_execution_weight(r.loop_depth);
    }

    /**
     * @brief Terminos de DERRAMAR el valor @p r, combinando las DOS fuentes:
     *        el peso de ejecucion (Facts del PROGRAMA) y el coste real de
     *        reload/store (Facts del HARDWARE via @c hw_cost).  Este metodo ES el
     *        punto de convergencia del modelo:
     *
     *            Facts-programa (execution_weight) + Facts-hardware (hw_cost)
     *                                    |
     *                              ObjectiveTerms  --> Candidate --> Decision
     *
     *        Trazabilidad ANAMNESIS: el termino @c spill resultante se explica
     *        como reload_latency(HW) x execution_weight(programa).
     */
    ObjectiveTerms spill_terms_for(const ValueRequirements &r) const noexcept {
        return spill_terms(r, execution_weight(r), hw_cost);
    }
};

/**
 * @brief Construye un contexto minimo con las piezas de Fase 0 + el coste HW.
 * @param hw  Fact del hardware (Tipo C) ya reducido a la @c SpillCostCard.  Por
 *            defecto el fallback generico (from_hw=false); el consumidor pasa la
 *            card real de @c spill_card_from(probe_machine_cost_facts(cost_model)).
 */
inline OptimizationContext make_context(const PhysicalRegisterBank &bank,
                                        const ConstraintSet &constraints,
                                        ObjectiveWeights objective = {},
                                        Budget budget = {},
                                        bool vec_reduction_active = false,
                                        SpillCostCard hw = {}) {
    OptimizationContext ctx;
    ctx.bank                 = &bank;
    ctx.constraints          = &constraints;
    ctx.objective            = objective;
    ctx.budget               = budget;
    ctx.vec_reduction_active = vec_reduction_active;
    ctx.hw_cost              = hw;
    return ctx;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_OPTIMIZATION_CONTEXT_H
