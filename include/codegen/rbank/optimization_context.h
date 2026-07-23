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
#include "jit/interval.h" // MachineNextUseFacts (Fact del nivel MachineIR) + codegen::LinearPos

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
 * @struct SpillTrace
 * @brief Contador (opcional, gated por el instrumento) de POR QUE se eligio cada
 *        victima de spill.  Responde la pregunta clave de Belady: ¿esta el
 *        allocator matando valores YA MUERTOS (next-use trivial) o eligiendo por
 *        USO MAS LEJANO?  Si casi todas las victimas son "dead", Belady no aporta
 *        mas que la heuristica de duracion; si hay muchas "alive", Belady trabaja.
 */
struct SpillTrace {
    uint64_t victims_dead  = 0; ///< victima SIN proximo uso (muerta -> Belady trivial).
    uint64_t victims_alive = 0; ///< victima con next-use finito (Belady real: uso lejano).
    uint64_t spills_total  = 0; ///< total de valores derramados.
    // Taxonomia: POR QUE existe cada spill (cierra el modelo explicativo).  La
    // rellena rbank_allocate con classify_spills sobre la asignacion PRE-recovery.
    uint64_t tax_fully      = 0; ///< lane libre en TODO su intervalo -> Recovery.
    uint64_t tax_partially  = 0; ///< lane libre en algun subintervalo -> Splitting.
    uint64_t tax_structural = 0; ///< nunca hay lane libre -> inevitable (overflow).
    uint64_t tax_splitting_potential = 0; ///< techo del splitting (area libre de los partially).
    // Recuperacion real de tax_fully.  KPI: Fully(limite superior) / Recovered(greedy)
    // / Potential = Fully - Recovered (fully que el greedy no llego a recuperar).
    uint64_t rec_greedy = 0; ///< spills recuperados por recover_spills (greedy).
    // Recuperacion real del SPLITTING (Fragmentation Recovery) sobre los partially.  El
    // KPI se lee contra tax_splitting_potential: Potential -> Recovered -> Remaining.
    uint64_t split_values     = 0; ///< valores con al menos un tramo recuperado.
    uint64_t split_intervals  = 0; ///< tramos del plan.
    uint64_t split_area       = 0; ///< area de lane devuelta a registro (posiciones).
    uint64_t split_uses       = 0; ///< usos que pasan a leer de registro.
    uint64_t split_rej_cost   = 0; ///< huecos descartados por el cost model.
    uint64_t split_rej_shape  = 0; ///< huecos descartados por las condiciones de forma.
    // Perfil de la decision (3c.5): sumas para derivar MEDIAS aceptado vs rechazado ->
    // permite justificar un cambio de parametro con datos, no moviendo constantes.
    int64_t  split_acc_gain   = 0; ///< suma de ganancia neta de los tramos aceptados.
    uint64_t split_rej_area   = 0; ///< area de los rechazados por coste.
    uint64_t split_rej_uses   = 0; ///< usos de los rechazados por coste.
    int64_t  split_rej_gain   = 0; ///< su ganancia neta (<=0): por cuanto no llegaron.

    void add(const SpillTrace &o) noexcept {
        victims_dead += o.victims_dead;
        victims_alive += o.victims_alive;
        spills_total += o.spills_total;
        tax_fully += o.tax_fully;
        tax_partially += o.tax_partially;
        tax_structural += o.tax_structural;
        tax_splitting_potential += o.tax_splitting_potential;
        rec_greedy += o.rec_greedy;
        split_values += o.split_values;
        split_intervals += o.split_intervals;
        split_area += o.split_area;
        split_uses += o.split_uses;
        split_rej_cost += o.split_rej_cost;
        split_acc_gain += o.split_acc_gain;
        split_rej_area += o.split_rej_area;
        split_rej_uses += o.split_rej_uses;
        split_rej_gain += o.split_rej_gain;
        split_rej_shape += o.split_rej_shape;
    }
};

/**
 * @struct OptimizationContext
 * @brief Firma estable que recibe la DecisionPolicy: punteros a las piezas +
 *        objetivo/presupuesto por valor.
 *
 * PRINCIPIO (no negociable): el contexto NO contiene conocimiento; contiene la
 * FORMA DE PREGUNTAR al conocimiento.  No es un almacen, es una INTERFAZ.  El
 * allocator llama @c ctx.next_use_distance() / @c ctx.spill_terms_for() sin saber
 * de donde sale el dato -> se puede cambiar por completo el backend de Facts sin
 * tocar ninguna politica.  Esto es lo que lo mantiene respirando a largo plazo.
 *
 * EVOLUCION (cuando crezca): separar CONFIGURACION (bank/constraints/objective/
 * budget) de CONOCIMIENTO (next_use/hw_cost/profile/loop/region/history).  Hoy el
 * conocimiento son 1-2 punteros sueltos; en cuanto lleguen profile/region/history
 * hay que agruparlos tras un @c ctx.knowledge.*() para que esto no derive en un
 * God Object.  El limite es CONCEPTUAL: config = "que problema resolver", knowledge
 * = "que se de del programa/hardware".
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

    // --- Fact del NIVEL MACHINEIR + trace (cableados por rbank_allocate) ---
    /// Next-use por vreg (Belady).  PUNTERO (no copia): lo computa el caller
    /// (@c compute_next_use(mf)) y lo cablea aqui.  nullptr = sin Belady -> el
    /// allocator cae a la heuristica de duracion restante.  Es la FACHADA de
    /// Knowledge: el allocator pide @c ctx.next_use_distance(), no sabe su origen.
    const jit::MachineNextUseFacts *next_use = nullptr;
    /// Trace opcional de la razon de cada victima de spill (instrumento; nullptr
    /// = sin conteo).  El allocator lo rellena si esta presente.
    SpillTrace *spill_trace = nullptr;

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
    /// ¿Hay Fact de next-use cableado (Belady disponible)?
    bool has_next_use() const noexcept { return next_use != nullptr; }
    /// Distancia al proximo uso del @p vreg desde @p now (dominio MachineIR).
    /// @c UINT32_MAX si el vreg no se vuelve a usar (muerto -> victima ideal
    /// Belady), o si no hay Fact cableado.
    /// DEUDA (consistencia): esto reintroduce un SENTINEL (UINT32_MAX), justo lo
    /// que @c codegen::LinearPos::invalid() vino a eliminar -- "el infinito no es una
    /// distancia".  Migrar a un tipo que exprese ausencia (optional<uint32_t> o un
    /// @c DistanceToNextUse con estado no-valido) cuando se toque la interfaz; el
    /// caller ya distingue el caso muerto con @c has_next_use() + el horizonte.
    uint32_t next_use_distance(uint32_t vreg, codegen::LinearPos now) const noexcept {
        return next_use ? next_use->distance_to_next_use(vreg, now) : UINT32_MAX;
    }
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
