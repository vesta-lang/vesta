/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file effect_analysis.cpp
 * @brief Esqueleto del gestor de analisis de efectos (Fase 0).  Cache local por
 *        nodo + summaries por funcion + invalidacion.  El mapeo real IrOp ->
 *        SemanticEffects (local) llega en Fase 1; el punto-fijo del callgraph
 *        (summary) en Fase 2.  Aqui devolvemos valores NEUTROS Complete para no
 *        alterar comportamiento (Fase 0: cero regresion).
 */
#include "vx/effects/effect_analysis.h"

#include "ir/ssa_ir.h"

namespace vx {
namespace fx {

EffectAnalysisResult EffectAnalysis::local(const ir::IrFunction & /*fn*/,
                                           const ir::IrInstr &ins) {
    const void *key = static_cast<const void *>(&ins);
    auto it = local_cache_.find(key);
    if (it != local_cache_.end()) return it->second;
    // Fase 0: neutro Complete (Fase 1 mapea IrOp -> SemanticEffects).
    EffectAnalysisResult r;
    local_cache_.emplace(key, r);
    return r;
}

FunctionSummary EffectAnalysis::compute_summary(const ir::IrModule & /*mod*/,
                                                const ir::IrFunction &fn) {
    FunctionSummary s;
    s.symbol = fn.name;
    // Fase 0: summary vacio Complete.  Fase 2 agrega bloques + punto-fijo.
    return s;
}

const FunctionSummary &EffectAnalysis::summary(const ir::IrModule &mod,
                                               const ir::IrFunction &fn) {
    auto d = dirty_.find(fn.name);
    const bool is_dirty = (d == dirty_.end()) || d->second;
    auto it = summary_cache_.find(fn.name);
    if (!is_dirty && it != summary_cache_.end()) return it->second;
    FunctionSummary s = compute_summary(mod, fn);
    dirty_[fn.name] = false;
    auto res = summary_cache_.insert_or_assign(fn.name, std::move(s));
    return res.first->second;
}

const ModuleSummary &EffectAnalysis::module_summary(const ir::IrModule &mod) {
    if (!module_dirty_) return module_cache_;
    module_cache_.fns.clear();
    for (const ir::IrFunction &fn : mod.functions)
        module_cache_.fns.emplace(fn.name, summary(mod, fn));
    module_dirty_ = false;
    return module_cache_;
}

void EffectAnalysis::invalidate_node(const ir::IrFunction &fn,
                                     const ir::IrInstr &ins) {
    local_cache_.erase(static_cast<const void *>(&ins));
    invalidate_function(fn.name);
}

void EffectAnalysis::invalidate_function(const std::string &fn_name) {
    dirty_[fn_name] = true;
    module_dirty_ = true;
    // TODO Fase 2: propagar a los callers transitivos por el callgraph (SCC).
}

void EffectAnalysis::clear() {
    local_cache_.clear();
    summary_cache_.clear();
    dirty_.clear();
    module_cache_.fns.clear();
    module_dirty_ = true;
}

} // namespace fx
} // namespace vx
