/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file contracts.cpp
 * @brief Generacion DECLARATIVA de contratos.  Cada contrato es una REGLA
 *        (nombre + predicado) registrada en una tabla.  Anadir un contrato
 *        nuevo = anadir una entrada aqui; el motor @c derive_contracts no
 *        cambia.  Los predicados leen el @c FunctionSummary completo (efectos
 *        local/closure + estructura + interproc), asi los contratos que
 *        necesitan analisis interprocedural o estructural tambien encajan.
 */
#include "analysis/effects/summary.h"

namespace analysis {
namespace effects {

// --------------------------------------------------------------------------
// Predicados.  Cada uno decide UN contrato desde el summary.  Son funciones
// libres (puntero de funcion, sin captura) para que la tabla sea estatica.
// --------------------------------------------------------------------------

/// pure: sin efectos de dato observables en TODO el cierre.  La definicion
/// depende del PERFIL (misma base de hechos, distinta opinion):
///   - Default : no escribe mem, no lanza/aloca/io/bloquea, sin tags.
///   - Strict  : ademas DETERMINISTA (sin lecturas de reloj/random/...).
///   - Relaxed : tolera may_trap (los traps 'no deberian pasar').
static bool p_pure(const FunctionSummary &s, ContractProfile profile) {
    if (s.completeness == AnalysisCompleteness::Unknown) return false;
    const SemanticEffects &c = s.semantic.closure;
    const bool base = !c.mem.writes_memory() && !c.may_throw &&
                      !c.may_allocate && !c.may_io && !c.may_block &&
                      c.tags.empty();
    if (!base) return false;
    if (profile == ContractProfile::Strict)
        return c.determinism.empty(); // Strict exige determinismo
    return true; // Default/Relaxed/Embedded
}

/// readonly: no escribe memoria en el cierre (puede leer).
static bool p_readonly(const FunctionSummary &s, ContractProfile profile) {
    return s.completeness != AnalysisCompleteness::Unknown &&
           !s.semantic.closure.mem.writes_memory();
}

/// leaf: no llama a nadie (ni estatica ni dinamicamente).
static bool p_leaf(const FunctionSummary &s, ContractProfile profile) {
    return !s.interproc.has_calls;
}

/// nothrow: no lanza en el cierre.
static bool p_nothrow(const FunctionSummary &s, ContractProfile profile) {
    return s.completeness != AnalysisCompleteness::Unknown &&
           !s.semantic.closure.may_throw;
}

/// nopanic: alias de nothrow para el FatalError de usuario (misma senal hoy).
static bool p_nopanic(const FunctionSummary &s, ContractProfile profile) { return p_nothrow(s, profile); }

/// deterministic: sin lecturas de reloj/random/pid/entorno ni I/O externa.
static bool p_deterministic(const FunctionSummary &s, ContractProfile profile) {
    const SemanticEffects &c = s.semantic.closure;
    return s.completeness != AnalysisCompleteness::Unknown &&
           c.determinism.empty() && !c.may_io;
}

/// heap_free: no aloca heap en el cierre.
static bool p_heap_free(const FunctionSummary &s, ContractProfile profile) {
    return s.completeness != AnalysisCompleteness::Unknown &&
           !s.semantic.closure.may_allocate;
}

/// gc_free: no aloca GC (hoy = no aloca heap; se afinara cuando el MemEffect
/// distinga GC de raw).
static bool p_gc_free(const FunctionSummary &s, ContractProfile profile) { return p_heap_free(s, profile); }

/// freestanding: no toca estado de maquina privilegiado ni I/O (candidato a Bare).
static bool p_freestanding(const FunctionSummary &s, ContractProfile profile) {
    const SemanticEffects &c = s.semantic.closure;
    const bool machine =
        c.tags.has(CapabilityTag::PortIO) || c.tags.has(CapabilityTag::MSR) ||
        c.tags.has(CapabilityTag::Privileged) ||
        c.tags.has(CapabilityTag::InterruptState);
    return s.completeness != AnalysisCompleteness::Unknown && !machine &&
           !c.may_io;
}

// --------------------------------------------------------------------------
// La TABLA.  Anadir un contrato = anadir una fila.  El motor no cambia.
// --------------------------------------------------------------------------
const std::vector<ContractRule> &contract_rules() {
    static const std::vector<ContractRule> rules = {
        {"pure", p_pure},
        {"readonly", p_readonly},
        {"leaf", p_leaf},
        {"nothrow", p_nothrow},
        {"nopanic", p_nopanic},
        {"deterministic", p_deterministic},
        {"heap_free", p_heap_free},
        {"gc_free", p_gc_free},
        {"freestanding", p_freestanding},
    };
    return rules;
}

std::vector<EvaluatedContract> derive_contracts(const FunctionSummary &s,
                                                ContractProfile profile) {
    std::vector<EvaluatedContract> out;
    const auto &rules = contract_rules();
    out.reserve(rules.size());
    for (const ContractRule &r : rules)
        out.push_back({r.name, r.predicate(s, profile)});
    return out;
}

} // namespace effects
} // namespace analysis
