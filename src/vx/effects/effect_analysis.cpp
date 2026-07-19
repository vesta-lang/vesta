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
#include "vx/effects/ir_effects.h"

namespace vx {
namespace fx {

EffectAnalysisResult EffectAnalysis::local(const ir::IrFunction &fn,
                                           const ir::IrInstr &ins) {
    const void *key = static_cast<const void *>(&ins);
    auto it = local_cache_.find(key);
    if (it != local_cache_.end()) return it->second;
    // El mapa def-use es por-funcion; en Fase 1 se reconstruye por llamada
    // (barato).  El agregado por-funcion usa function_local_effects, que lo
    // construye una vez.  Fase 2 cachea el def-map por funcion.
    IrDefMap defs = build_def_map(fn);
    EffectAnalysisResult r = effects_of_instr(fn, defs, ins);
    local_cache_.emplace(key, r);
    return r;
}

// Extrae la faceta ESTRUCTURAL de una funcion (bloques, back-edges = bucles,
// recursion directa).  El detalle de trip-counts (para Big-O) lo afina el
// subsistema de coste; aqui damos la forma.
static StructuralSummary structural_of(const ir::IrFunction &fn) {
    StructuralSummary st;
    st.block_count = static_cast<uint32_t>(fn.blocks.size());
    // Back-edge = arista a un bloque de indice <= el actual (aproximacion de
    // bucle sobre el orden de bloques).  Recursion directa = se llama a si misma.
    for (uint32_t bi = 0; bi < fn.blocks.size(); ++bi) {
        for (const ir::IrInstr &in : fn.blocks[bi].instrs) {
            auto is_back = [&](ir::IrBlockId t) {
                return t != ir::IR_NO_BLOCK && t <= bi;
            };
            if (in.op == ir::IrOp::BR) {
                if (is_back(in.target_block)) ++st.loop_count;
            } else if (in.op == ir::IrOp::BR_COND) {
                if (is_back(in.target_block) || is_back(in.false_block))
                    ++st.loop_count;
            } else if (in.op == ir::IrOp::SWITCH_DENSE ||
                       in.op == ir::IrOp::MATCH_VARIANT) {
                for (uint32_t t : in.jump_targets)
                    if (is_back(t)) { ++st.loop_count; break; }
            }
            if ((in.op == ir::IrOp::CALL || in.op == ir::IrOp::TAILCALL) &&
                in.func_name == fn.name)
                st.recursive = true;
        }
    }
    if (st.loop_count > 0) st.has_unbounded_loop = true; // conservador sin trip-count
    return st;
}

FunctionSummary EffectAnalysis::compute_summary(const ir::IrModule & /*mod*/,
                                                const ir::IrFunction &fn) {
    // Summary SIN cierre (se usa como fallback; el cierre real lo calcula
    // module_summary por punto-fijo).  compute_summary se mantiene para el
    // acceso por-funcion aislado.
    FunctionSummary s;
    s.symbol = fn.name;
    EffectAnalysisResult loc = function_local_effects(fn);
    s.semantic.local = loc.effects;
    s.semantic.closure = loc.effects; // sin interproc; module_summary lo completa
    s.structural = structural_of(fn);
    s.completeness = loc.completeness;
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

// Callees de una funcion: nombres estaticos (CALL/TAILCALL) + si hace alguna
// llamada DINAMICA/nativa (callee desconocido -> el cierre toma el efecto TOP
// robusto: puede hacer cualquier cosa, con el motivo registrado para el reporte).
namespace {
struct CallInfo {
    std::vector<std::string> static_callees;
    bool                     dynamic = false; // CALLVIRT/CALLIND/CALLN/...
};
CallInfo callees_of(const ir::IrFunction &fn) {
    CallInfo ci;
    for (const ir::IrBlock &b : fn.blocks)
        for (const ir::IrInstr &in : b.instrs) {
            switch (in.op) {
            case ir::IrOp::CALL:
            case ir::IrOp::TAILCALL:
                if (!in.func_name.empty()) ci.static_callees.push_back(in.func_name);
                else ci.dynamic = true;
                break;
            case ir::IrOp::CALLVIRT:
            case ir::IrOp::CALLM:
            case ir::IrOp::CALLITF:
            case ir::IrOp::CALLCLOSURE:
            case ir::IrOp::CALLIND:
            case ir::IrOp::CALLN:
                ci.dynamic = true;
                break;
            default:
                break;
            }
        }
    return ci;
}
// Une el cierre del callee en el del caller (sin remapeo fino de ArgDerived:
// v1 lo trata como Unknown -- conservador y sound).
void merge_callee(SemanticEffects &caller, const SemanticEffects &callee) {
    caller = join(caller, callee);
}
} // namespace

const ModuleSummary &EffectAnalysis::module_summary(const ir::IrModule &mod) {
    if (!module_dirty_) return module_cache_;
    module_cache_.fns.clear();

    // 1) Summary LOCAL de cada funcion (efecto propio, estructura) + lagunas.
    gaps_ = EffectGaps{};
    std::unordered_map<std::string, CallInfo> calls;
    for (const ir::IrFunction &fn : mod.functions) {
        EffectAnalysisResult loc = function_local_effects(fn, &gaps_);
        FunctionSummary s;
        s.symbol = fn.name;
        s.semantic.local = loc.effects;
        s.semantic.closure = loc.effects; // arranca = local; el fixpoint lo expande
        s.structural = structural_of(fn);
        s.completeness = loc.completeness;
        calls[fn.name] = callees_of(fn);
        s.interproc.reaches_dynamic_call = calls[fn.name].dynamic;
        s.interproc.has_calls =
            calls[fn.name].dynamic || !calls[fn.name].static_callees.empty();
        module_cache_.fns.emplace(fn.name, std::move(s));
    }

    // 2) Punto-fijo: closure(fn) = local(fn) U closure(callee) para cada callee.
    //    Una llamada dinamica/nativa o a una funcion externa (no en el modulo)
    //    hace el cierre CONSERVADOR (puede tocar mem, lanzar, alocar, I/O).
    bool changed = true;
    int guard = 0;
    const int max_iters = static_cast<int>(mod.functions.size()) + 4;
    while (changed && guard++ < max_iters * 2 + 8) {
        changed = false;
        for (const ir::IrFunction &fn : mod.functions) {
            FunctionSummary &s = module_cache_.fns[fn.name];
            SemanticEffects nc = s.semantic.local;
            AnalysisCompleteness comp = s.completeness;
            const CallInfo &ci = calls[fn.name];
            // Una llamada dinamica/nativa (CALLVIRT/CALLN/...) no se puede acotar:
            // el cierre toma el efecto TOP robusto (el local ya lo incluye via
            // effects_of_instr, pero lo reforzamos aqui por si el local no lo
            // subio del todo).
            if (ci.dynamic) {
                nc = join(nc, SemanticEffects::top());
                if (comp == AnalysisCompleteness::Complete)
                    comp = AnalysisCompleteness::Conservative;
            }
            for (const std::string &callee : ci.static_callees) {
                auto it = module_cache_.fns.find(callee);
                if (it == module_cache_.fns.end()) {
                    // Callee EXTERNO al modulo -> efecto TOP robusto (fundamental).
                    nc = join(nc, SemanticEffects::top());
                    if (comp == AnalysisCompleteness::Complete)
                        comp = AnalysisCompleteness::Conservative;
                    continue;
                }
                merge_callee(nc, it->second.semantic.closure);
                if (uint8_t(it->second.completeness) > uint8_t(comp))
                    comp = it->second.completeness;
            }
            if (!(nc == s.semantic.closure) || comp != s.completeness) {
                s.semantic.closure = nc;
                s.completeness = comp;
                changed = true;
            }
        }
    }

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
