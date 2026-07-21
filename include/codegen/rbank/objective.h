/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/objective.h
 * @brief Nivel 3 del modelo de asignacion de recursos: PREFERENCIAS SUAVES.
 *
 * El @c Objective es lo OPUESTO a @c Constraints: si las restricciones dicen
 * que combinaciones son FACTIBLES (duro, booleano), el objetivo dice cuales son
 * MEJORES (suave, escalar).  Entre todas las soluciones validas, la
 * @c DecisionPolicy elegira la de menor COSTE segun este objetivo.
 *
 *     PhysicalRegisterBank -> ValueRequirements -> Constraints
 *                                                      |
 *                                                  Objective   <-- este modulo
 *                                                      |
 *                                    OptimizationContext -> DecisionPolicy
 *
 * DOS PARTES:
 *   1. FRAMEWORK de scoring: @c ObjectiveWeights (pesos por dimension, con un
 *      default determinista) + @c ObjectiveTerms (los terminos de un candidato)
 *      + @c objective_score (suma ponderada; MENOR = MEJOR, es un COSTE).
 *      Multi-dimensional: latencia, throughput, tamano de codigo, energia,
 *      presion de cache, presion de registros, spill, mov, callsave.
 *   2. ESTIMADORES puros que interpretan los HECHOS de @c ValueRequirements
 *      como preferencias del allocator: hotness (loop_depth) encarece el spill;
 *      rematerializable lo abarata; cruzar un CALL en una lane VOLATILE cuesta
 *      salvar/restaurar en cada call (preferir PRESERVED); romper un tie de
 *      two-address cuesta un mov.
 *
 * LIMITE (que NO va aqui): las LATENCIAS/PUERTOS reales por instruccion vienen
 * de arch-data (jit/sched/cost_model.h -> AsmCost) y se inyectan en el termino
 * @c latency/@c throughput durante el wiring de Fase 0.25.  Aqui viven el
 * FRAMEWORK y los estimadores especificos del allocator (spill/callsave/move),
 * que son ISA-neutrales.
 *
 * i18n: el objetivo produce NUMEROS (scores), no diagnosticos -> sin catalogo.
 * El @c explain() de la @c DecisionPolicy (nivel siguiente) SI usara el catalogo
 * i18n para justificar una decision al usuario.
 *
 * Fase 0: ADITIVO, funciones puras, sin consumidores (solo el test).
 */

#ifndef VESTA_CODEGEN_RBANK_OBJECTIVE_H
#define VESTA_CODEGEN_RBANK_OBJECTIVE_H

#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/value_requirements.h"

#include <cstdint>

namespace jit {
namespace rbank {

/**
 * @struct ObjectiveWeights
 * @brief Pesos de cada dimension del coste.  El default es determinista y
 *        conservador; @c OptimizationContext podra reajustarlos por perfil o
 *        presupuesto, y un Predictor (MLGO futuro) aprenderlos.
 */
struct ObjectiveWeights {
    double latency           = 1.0;  ///< coste de latencia (ciclos ruta critica).
    double throughput        = 1.0;  ///< coste de throughput (uops/ciclo).
    double code_size         = 0.5;  ///< bytes de codigo emitidos.
    double energy            = 0.0;  ///< energia (off por defecto).
    double cache_pressure    = 0.5;  ///< presion de cache de datos (spills en pila).
    double register_pressure = 0.25; ///< presion de registros (lanes ocupadas).
    double spill             = 4.0;  ///< un spill (load/store extra por uso).
    double move              = 1.0;  ///< un mov (tie de two-address roto).
    double callsave          = 2.0;  ///< salvar/restaurar alrededor de un CALL.
    // Dimensiones del scheduler (arch-data las afinara; comparten Objective):
    double dependency        = 1.0;  ///< stalls por cadena de dependencias.
    double port_pressure     = 0.5;  ///< contencion de puertos de ejecucion.
    double scheduler         = 0.0;  ///< dificultad de scheduling (meta; off).
};

/**
 * @struct ObjectiveTerms
 * @brief Terminos de coste de un candidato (misma base que los pesos).  Los
 *        rellenan los estimadores + arch-data.  @c objective_score los agrega.
 */
struct ObjectiveTerms {
    double latency           = 0.0;
    double throughput        = 0.0;
    double code_size         = 0.0;
    double energy            = 0.0;
    double cache_pressure    = 0.0;
    double register_pressure = 0.0;
    double spill             = 0.0;
    double move              = 0.0;
    double callsave          = 0.0;
    double dependency        = 0.0;  ///< stalls por dependencias (scheduler).
    double port_pressure     = 0.0;  ///< contencion de puertos (scheduler).
    double scheduler         = 0.0;  ///< dificultad de scheduling (meta).

    /** @brief Suma componente a componente (agregar candidatos parciales). */
    ObjectiveTerms &operator+=(const ObjectiveTerms &o) noexcept {
        latency += o.latency;       throughput += o.throughput;
        code_size += o.code_size;   energy += o.energy;
        cache_pressure += o.cache_pressure;
        register_pressure += o.register_pressure;
        spill += o.spill;           move += o.move;   callsave += o.callsave;
        dependency += o.dependency; port_pressure += o.port_pressure;
        scheduler += o.scheduler;
        return *this;
    }
};

/**
 * @brief Coste escalar de un candidato: suma ponderada (MENOR = MEJOR).
 */
inline double objective_score(const ObjectiveTerms &t,
                              const ObjectiveWeights &w) noexcept {
    return w.latency * t.latency + w.throughput * t.throughput +
           w.code_size * t.code_size + w.energy * t.energy +
           w.cache_pressure * t.cache_pressure +
           w.register_pressure * t.register_pressure +
           w.spill * t.spill + w.move * t.move + w.callsave * t.callsave +
           w.dependency * t.dependency + w.port_pressure * t.port_pressure +
           w.scheduler * t.scheduler;
}

// ---------------------------------------------------------------------------
//  Estimadores puros (interpretan los HECHOS como preferencias).  Heuristicos
//  hasta que arch-data afine los terminos de latencia/throughput en Fase 0.25.
// ---------------------------------------------------------------------------

/**
 * @brief Peso de ejecucion ESTATICO (fallback) de una profundidad de loop.
 *
 * Es solo una HEURISTICA por defecto (cada nivel de loop ~x10).  El peso REAL
 * debe SUMINISTRARLO el @c OptimizationContext: con perfil da una frecuencia
 * medida (p.ej. 125.3), con PGO otra (4890); atar el modelo a @c 10^depth seria
 * un error.  Por eso los estimadores reciben el peso como PARAMETRO y esta
 * funcion es solo el valor por defecto cuando no hay perfil.
 */
inline double static_execution_weight(uint16_t loop_depth) noexcept {
    const uint16_t d = loop_depth > 9 ? 9 : loop_depth; // cota 10^9.
    double f = 1.0;
    for (uint16_t i = 0; i < d; ++i) f *= 10.0;
    return f;
}

/**
 * @brief Coste estimado de DERRAMAR un valor a memoria (por uso).
 * @param exec_weight  peso de ejecucion (lo suministra el contexto: perfil/PGO,
 *        o @c static_execution_weight(r.loop_depth) como fallback).
 *
 * Hoy @c base * @c exec_weight con @c base menor si es rematerializable.  El
 * framework ya admite la formula completa (@c load_latency + @c store_latency +
 * @c cache_pressure + @c frequency + @c remat): esos terminos se suman via
 * @c ObjectiveTerms cuando arch-data los aporte (Fase 0.25).
 */
inline double spill_cost_of(const ValueRequirements &r,
                            double exec_weight) noexcept {
    const double base = r.rematerializable ? 1.0 : 3.0; // remat < load.
    return base * exec_weight;
}

/**
 * @brief Coste estimado de salvar/restaurar un valor que cruza CALLs.
 * @param sp        politica de la lane candidata (VOLATILE/PRESERVED/RESERVED).
 * @param n_calls   numero de call-sites que el valor cruza.
 *
 * PRESERVED: coste UNICO (push/pop en prologo/epilogo).  VOLATILE: hay que
 * salvar/restaurar alrededor de CADA call -> escala con @p n_calls.  Si el
 * valor no cruza calls, cero (preferencia neutra).
 */
inline double callsave_cost_of(const ValueRequirements &r, SavePolicy sp,
                              uint32_t n_calls) noexcept {
    if (!r.crosses_call) return 0.0;
    if (sp == SavePolicy::PRESERVED) return 2.0;      // una vez.
    return 2.0 * static_cast<double>(n_calls);        // alrededor de cada call.
}

/** @brief Coste de un mov (tie de two-address roto: dst != src1). */
inline double move_cost() noexcept { return 1.0; }

/**
 * @brief Terminos de objetivo de ASIGNAR el valor @p r a la lane @p lane.
 * @param n_calls  call-sites que el valor cruza (para el termino callsave).
 *
 * Rellena los terminos que dependen de la ELECCION de lane: @c callsave (segun
 * la SavePolicy de la lane) y @c register_pressure (ocupa una lane).  El resto
 * de terminos (latencia/throughput/code_size) los aporta el codegen/arch-data.
 */
inline ObjectiveTerms lane_choice_terms(const ValueRequirements &r,
                                        const Lane &lane,
                                        uint32_t n_calls) noexcept {
    ObjectiveTerms t;
    // Ocupa una lane.  NOTA: hoy 1.0 uniforme; en el futuro la presion sera
    // POR-CLASE (GP/FP/MASK/PRED tienen escasez distinta) -> el peso lo dara la
    // presion relativa de la clase en el banco.  El framework ya lo permite.
    t.register_pressure = 1.0;
    t.callsave = callsave_cost_of(r, lane.preservation_of(r.width), n_calls);
    return t;
}

/**
 * @brief Terminos de objetivo de DERRAMAR el valor @p r (no ocupa lane).
 * @param exec_weight  peso de ejecucion suministrado por el contexto.
 */
inline ObjectiveTerms spill_terms(const ValueRequirements &r,
                                  double exec_weight) noexcept {
    ObjectiveTerms t;
    t.spill = spill_cost_of(r, exec_weight);
    t.cache_pressure = 1.0; // un slot de pila mas.
    return t;
}

} // namespace rbank
} // namespace jit

#endif // VESTA_CODEGEN_RBANK_OBJECTIVE_H
