/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/function_snapshot.h
 * @brief FunctionSnapshot: el ESTADO DE CONOCIMIENTO del compilador sobre una
 *        funcion.  Objeto de PRIMER NIVEL que consumen los 3 modos (interp/JIT/
 *        AOT) y todo motor de decision.
 *
 * Deja de ser "info del allocator": es LITERALMENTE la fotografia del programa.
 * En vez de "¿como calculo los loops?" preguntas @c snapshot.loops; en vez de
 * "¿es hot?" preguntas @c snapshot.profile.weight_of(...).  Cada consumidor
 * (scheduler, allocator, vectorizer, LICM, SROA, inlining) lee la MISMA foto ->
 * cero duplicacion.
 *
 * Arquitectura (el snapshot como repositorio de conocimiento; escala anadiendo
 * Facts SIN tocar consumidores):
 *
 *                          IrFunction (codigo real)
 *                                   |
 *                           build_snapshot(fn)
 *                                   |
 *        +----------------+---------+---------+----------------+
 *        v                v                   v                v
 *   LivenessResult   LoopFacts          ProfileFacts     ValueRequirements[]
 *    (Tipo A)        (Tipo A)           (Tipo B)          (adaptadores)
 *        |                |                   |                |
 *        +----------------+--------+----------+----------------+
 *                                  |
 *                          FunctionSnapshot   <-- objeto de 1er nivel
 *                                  |
 *        +----------------+--------+---------+-----------------+
 *        v                v                  v                 v
 *     interp           JIT               AOT            scheduler/allocator/
 *     (los 3 modos leen la MISMA foto)                  vectorizer/LICM/...
 *
 * FUTURO (crece sin cambiar consumidores; cada Fact entra cuando tenga un
 * consumidor real): DomFacts, AliasFacts, EscapeFacts, OwnershipFacts,
 * ExceptionFacts, VectorFacts, LifetimeFacts, ConcurrencyFacts, GCFacts.
 *
 * AUTOCERTIFICACION: cada Fact se valida a SI MISMO (@c analysis::validate) y el
 * auditor comprueba que ningun valor es imposible.  @c FunctionSnapshot::validate
 * agrega todo -> el compilador se AUDITA antes de usar el conocimiento:
 *
 *      validate(loops) + validate(profile) + liveness_check + audit_requirements
 *                                   |
 *                            ValidationReport   (ok() = conocimiento sano)
 */

#ifndef VESTA_CODEGEN_RBANK_FUNCTION_SNAPSHOT_H
#define VESTA_CODEGEN_RBANK_FUNCTION_SNAPSHOT_H

#include "analysis/derived/profile_facts.h"
#include "analysis/fact_validation.h"
#include "analysis/facts/loop_facts.h"
#include "codegen/rbank/build_requirements.h"
#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/value_requirements.h"
#include "ir/liveness.h"
#include "ir/ssa_ir.h"

#include <vector>

namespace jit {
namespace rbank {

/**
 * @struct FunctionSnapshot
 * @brief Fotografia del conocimiento del compilador sobre una funcion.
 *
 * Los Facts son de SOLO LECTURA tras construirse (una fotografia congelada).
 * Crece anadiendo campos (DomFacts, AliasFacts, ...) sin cambiar consumidores.
 */
struct FunctionSnapshot {
    const ir::IrFunction  *fn = nullptr; ///< funcion de la que es foto.
    ir::LivenessResult     live;         ///< Tipo A: intervalos de vida.
    analysis::LoopFacts    loops;        ///< Tipo A: bucles.
    analysis::ProfileFacts profile;      ///< Tipo B: perfil (vacio si no hay).
    std::vector<ValueRequirements> values; ///< requisitos por valor (adaptadores).
    // Futuro: analysis::DomFacts dom;  analysis::AliasFacts alias;  ...

    /**
     * @struct ValidationReport
     * @brief Resultado de la autocertificacion agregada del snapshot (DATOS).
     */
    struct ValidationReport {
        std::vector<analysis::FactIssue> fact_issues;  ///< de Loop/Profile/Liveness.
        std::vector<RequirementIssue>    value_issues; ///< valores imposibles.
        bool ok() const noexcept {
            return fact_issues.empty() && value_issues.empty();
        }
    };

    /**
     * @brief AUDITA el snapshot: autocertifica cada Fact + comprueba que ningun
     *        valor es imposible en @p bank.  El compilador auditandose.
     */
    ValidationReport validate(const PhysicalRegisterBank &bank,
                              bool vec_reduction_active = false) const {
        ValidationReport rep;
        // Autocertificacion de cada Fact.
        for (const analysis::FactIssue &i : analysis::validate(loops))
            rep.fact_issues.push_back(i);
        for (const analysis::FactIssue &i : analysis::validate(profile))
            rep.fact_issues.push_back(i);
        // Liveness: def <= end en todo intervalo.
        for (const ir::LiveInterval &iv : live.intervals)
            if (iv.def > iv.end)
                rep.fact_issues.push_back(
                    {analysis::FactCheck::LIVE_DEF_AFTER_END, iv.id, 0});
        // Auditor de valores imposibles.
        rep.value_issues = audit_requirements(values, bank, vec_reduction_active);
        return rep;
    }
};

/**
 * @brief Construye la fotografia completa de @p fn: Facts + ValueRequirements.
 * @param prof  perfil de branches (opcional).
 */
inline FunctionSnapshot build_snapshot(
    const ir::IrFunction &fn,
    const analysis::BranchProfile *prof = nullptr) {

    FunctionSnapshot s;
    s.fn    = &fn;
    s.live  = ir::compute_liveness(fn);
    s.loops = analysis::compute_loop_facts(fn);
    if (prof && !prof->empty())
        s.profile = analysis::compute_profile_facts(fn, s.loops, *prof);

    std::vector<uint32_t> calls = collect_call_positions(fn, s.live);
    s.values = assemble_value_requirements(fn, s.live, calls, s.loops, s.profile);
    return s;
}

} // namespace rbank
} // namespace jit

#endif // VESTA_CODEGEN_RBANK_FUNCTION_SNAPSHOT_H
