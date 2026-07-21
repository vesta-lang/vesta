/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/function_snapshot.h
 * @brief FunctionSnapshot: el REPOSITORIO DE CONOCIMIENTO del compilador sobre
 *        una funcion, con QUERY SYSTEM lazy (demand-driven, estilo rustc).
 *
 * Vesta deja de ser una cadena de passes.  Tiene DOS representaciones del
 * programa: el IR y el CONOCIMIENTO (Programa -> Hechos -> Decisiones).  El
 * @c FunctionSnapshot es ese conocimiento: un objeto de PRIMER NIVEL que
 * consultan los 3 modos (interp/JIT/AOT) y todo motor de decision.
 *
 *      IR
 *       |
 *       v
 *   Repositorio de conocimiento (FunctionSnapshot)   <-- este objeto
 *       |
 *       +--> interp / JIT / AOT / scheduler / allocator / vectorizer / ...
 *       |
 *       v
 *   Autovalidacion (cada Fact se autocertifica; el snapshot se AUDITA)
 *
 * QUERY SYSTEM (lazy + cache): no se construye todo de golpe.  Cada hecho se
 * COMPUTA LA PRIMERA VEZ que se pide, se CACHEA y se devuelve.  Las dependencias
 * se resuelven SOLAS porque los accessors se llaman entre si:
 *
 *      value_reqs()  --calls-->  liveness(), loop_facts(), profile_facts()
 *      profile_facts() --calls--> loop_facts()
 *
 *      snapshot.loop_facts()      // si no existe -> compute_loop_facts -> cache
 *      snapshot.value_reqs()      // arrastra liveness + loops + profile
 *
 * Los campos-cache son publicos (para serializacion / warm-load / inspeccion);
 * la interfaz RECOMENDADA es la de accessors (demand-driven).  Se construye
 * eager un subconjunto con @c SnapshotBuilder (snapshot_builder.h) cuando
 * interese (p.ej. para serializar).
 *
 * CRECE sin tocar consumidores: cada Fact nuevo (DomFacts/AliasFacts/Escape/
 * Memory/...) es un campo-cache + un accessor + un bit en @c Fact.
 *
 * SERIALIZACION (futura, cuando haya consumidor): al ser DATOS, el snapshot es
 * serializable -> IR -> Snapshot -> cache.  Casi un "core dump" del conocimiento:
 * reproducir bugs, comparar snapshots entre versiones, regresion de analisis,
 * herramientas externas.
 *
 * ESCALADO (mismo modelo mental, sin cambiarlo; cada nivel cuando tenga
 * consumidor real -- NO por prevision):
 *
 *      ValueRequirements   (valor)
 *            ^
 *      FunctionSnapshot    (funcion)   <-- HOY
 *            ^
 *      ModuleSnapshot      (modulo: Functions + Globals + CallGraph + alias/
 *            ^              escape/ownership GLOBALES + PGO + Inlining)
 *      ProgramSnapshot     (programa: LTO)
 *
 * Que el mismo objeto escale de valor -> funcion -> modulo -> programa sin
 * cambiar el modelo (Facts lazy + autocertificacion + query) es senal de que la
 * arquitectura es correcta.
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

#include <cstdint>
#include <vector>

namespace jit {
namespace rbank {

/**
 * @enum Fact
 * @brief Que hechos puede contener/computar el snapshot (mascara de bits).
 *        Crece anadiendo entradas (Dom/Alias/Escape/Memory...) sin tocar el
 *        punto de entrada ni los consumidores.
 */
enum class Fact : uint32_t {
    None     = 0,
    Liveness = 1u << 0, ///< intervalos de vida (Tipo A).
    Loops    = 1u << 1, ///< LoopFacts (Tipo A).
    Profile  = 1u << 2, ///< ProfileFacts (Tipo B; depende de Loops + perfil).
    Values   = 1u << 3, ///< ValueRequirements (adaptadores; dep. Liveness+Loops).
    // Futuro: Dom = 1u<<4, Alias = 1u<<5, Escape = 1u<<6, Memory = 1u<<7, ...
    All      = Liveness | Loops | Profile | Values,
};

/**
 * @struct FunctionSnapshot
 * @brief Repositorio de conocimiento de una funcion con query system lazy.
 *
 * Los campos-cache son @c mutable: los accessors @c const los rellenan la
 * primera vez (demand-driven).  @c computed marca que hechos ya estan.
 */
struct FunctionSnapshot {
    const ir::IrFunction          *fn   = nullptr; ///< funcion de la que es foto.
    const analysis::BranchProfile *prof = nullptr; ///< perfil (debe sobrevivir al snapshot si Profile es lazy).

    // --- Campos-cache (publicos para serializacion/inspeccion; preferir accessors) ---
    mutable ir::LivenessResult             live;    ///< Tipo A.
    mutable analysis::LoopFacts            loops;   ///< Tipo A.
    mutable analysis::ProfileFacts         profile; ///< Tipo B (vacio si no hay perfil).
    mutable std::vector<ValueRequirements> values;  ///< requisitos por valor.
    mutable uint32_t                       computed = 0; ///< mascara de @c Fact ya computados.
    // Futuro: mutable analysis::DomFacts dom;  mutable analysis::AliasFacts alias; ...

    bool is_computed(Fact f) const noexcept {
        return (computed & static_cast<uint32_t>(f)) != 0;
    }

    // --- QUERY SYSTEM (lazy + cache; resuelve dependencias al llamarse entre si) ---

    /** @brief Intervalos de vida (compute-si-falta + cache). */
    const ir::LivenessResult &liveness() const {
        if (!is_computed(Fact::Liveness)) {
            live = ir::compute_liveness(*fn);
            computed |= static_cast<uint32_t>(Fact::Liveness);
        }
        return live;
    }
    /** @brief LoopFacts (compute-si-falta + cache). */
    const analysis::LoopFacts &loop_facts() const {
        if (!is_computed(Fact::Loops)) {
            loops = analysis::compute_loop_facts(*fn);
            computed |= static_cast<uint32_t>(Fact::Loops);
        }
        return loops;
    }
    /** @brief ProfileFacts (compute-si-falta + cache; vacio si no hay perfil). */
    const analysis::ProfileFacts &profile_facts() const {
        if (!is_computed(Fact::Profile)) {
            if (prof && !prof->empty())
                profile = analysis::compute_profile_facts(*fn, loop_facts(), *prof);
            computed |= static_cast<uint32_t>(Fact::Profile); // marca aunque quede vacio
        }
        return profile;
    }
    /** @brief ValueRequirements (arrastra liveness + loops + profile). */
    const std::vector<ValueRequirements> &value_reqs() const {
        if (!is_computed(Fact::Values)) {
            std::vector<uint32_t> calls = collect_call_positions(*fn, liveness());
            values = assemble_value_requirements(*fn, liveness(), calls,
                                                 loop_facts(), profile_facts());
            computed |= static_cast<uint32_t>(Fact::Values);
        }
        return values;
    }

    /**
     * @struct ValidationReport
     * @brief Resultado de la autocertificacion agregada (DATOS).
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
     *        valor es imposible en @p bank.  El compilador auditandose (fuerza
     *        el computo de los hechos que audita).
     */
    ValidationReport validate(const PhysicalRegisterBank &bank,
                              bool vec_reduction_active = false) const {
        ValidationReport rep;
        for (const analysis::FactIssue &i : analysis::validate(loop_facts()))
            rep.fact_issues.push_back(i);
        for (const analysis::FactIssue &i : analysis::validate(profile_facts()))
            rep.fact_issues.push_back(i);
        for (const ir::LiveInterval &iv : liveness().intervals)
            if (iv.def > iv.end)
                rep.fact_issues.push_back(
                    {analysis::FactCheck::LIVE_DEF_AFTER_END, iv.id, 0});
        rep.value_issues = audit_requirements(value_reqs(), bank, vec_reduction_active);
        return rep;
    }
};

} // namespace rbank
} // namespace jit

#endif // VESTA_CODEGEN_RBANK_FUNCTION_SNAPSHOT_H
