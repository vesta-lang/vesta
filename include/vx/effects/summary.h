/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/effects/summary.h
 * @brief Resumenes por-funcion y por-modulo del sistema de efectos, y la
 *        generacion DECLARATIVA de contratos.  El @c FunctionSummary es la capa
 *        de analisis compartida (efectos + estructura + interproc) computada UNA
 *        vez, de la que TODO producto (contratos, complejidad, autodoc,
 *        diagramas, API, optimizer) es proyeccion pura.
 */
#ifndef VX_EFFECTS_SUMMARY_H
#define VX_EFFECTS_SUMMARY_H

#include "vx/effects/effects.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace vx {
namespace fx {

/// Faceta SEMANTICA de una funcion: efecto local + efecto transitivo (cierre).
struct SemanticSummary {
    SemanticEffects local;   ///< agregado de los bloques de la funcion.
    SemanticEffects closure; ///< cierre transitivo por el callgraph.
};

/// Faceta ESTRUCTURAL (para complejidad).  Big-O NO se deriva de efectos; vive
/// aqui.  Esqueleto en Fase 0; lo puebla el subsistema de coste en Fase 2.
struct StructuralSummary {
    uint32_t block_count = 0;
    uint32_t loop_count = 0;
    uint32_t max_loop_depth = 0;
    bool     recursive = false;
    bool     has_unbounded_loop = false; ///< algun bucle sin trip-count acotable.
};

/// Faceta INTERPROCEDURAL: agregados numericos del cierre del callgraph.
struct InterprocSummary {
    int64_t alloc_total = -1;      ///< sitios de alloc alcanzables (-1 = desconocido).
    int64_t stack_peak_total = -1; ///< profundidad de pila peor caso.
    bool    reaches_dynamic_call = false;
};

/// Resumen COMPLETO por funcion (contenedor de las 3 facetas).
struct FunctionSummary {
    SemanticSummary      semantic;
    StructuralSummary    structural;
    InterprocSummary     interproc;
    AnalysisCompleteness completeness = AnalysisCompleteness::Complete;
    std::string          symbol;
    bool                 exported = false;
};

/// Nivel modulo = MAPA symbol -> summary (NO un efecto de modulo).
struct ModuleSummary {
    std::unordered_map<std::string, FunctionSummary> fns;
};

// ===========================================================================
// Contratos DECLARATIVOS.  Un contrato es una REGLA con
// nombre + predicado sobre el FunctionSummary completo.  Anadir un contrato =
// registrar una regla; NO se toca el motor.
// ===========================================================================
struct ContractRule {
    const char *name;
    bool (*predicate)(const FunctionSummary &);
};

/// Registro global de reglas (definidas en contracts.cpp).  Devuelve el vector
/// estable de reglas registradas.
const std::vector<ContractRule> &contract_rules();

/// Contrato evaluado: nombre + si se cumple.
struct EvaluatedContract {
    const char *name;
    bool        holds;
};

/// Proyeccion pura: aplica TODAS las reglas al summary.
std::vector<EvaluatedContract> derive_contracts(const FunctionSummary &s);

} // namespace fx
} // namespace vx

#endif // VX_EFFECTS_SUMMARY_H
