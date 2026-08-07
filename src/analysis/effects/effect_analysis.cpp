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
 *        alterar comportamiento (  cero regresion).
 */
#include "analysis/effects/effect_analysis.h"

#include "ir/ssa_ir.h"
#include "analysis/effects/ir_effects.h"
#include "analysis/escape/escape.h"

#include <deque>
#include <unordered_set>
#include <utility>

namespace analysis {
namespace effects {

const IrFacts &EffectAnalysis::facts_of(const ir::IrFunction &fn) {
    // Hechos fundacionales cacheados por el AnalysisManager: se computan una vez
    // por funcion y se reusan (antes se reconstruian en cada consulta local ->
    // O(n^2) por funcion; ahora O(n)).
    return facts_mgr_.get_or_compute<IRFactsAnalysis, IrFacts>(
        fn.name, [&]() { return build_ir_facts(fn); });
}

const PointsTo &EffectAnalysis::points_to_of(const ir::IrFunction &fn) {
    // Tabla points-to cacheada; su factory consume facts_of (via el manager),
    // registrando la dependencia PointsTo -> IRFacts para la invalidacion.
    return facts_mgr_.get_or_compute<PointsToAnalysis, PointsTo>(
        fn.name, [&]() { return compute_points_to(fn, facts_of(fn)); });
}

EffectAnalysisResult EffectAnalysis::local(const ir::IrFunction &fn,
                                           const ir::IrInstr &ins) {
    const void *key = static_cast<const void *>(&ins);
    auto it = local_cache_.find(key);
    if (it != local_cache_.end()) return it->second;
    EffectAnalysisResult r = effects_of_instr(fn, facts_of(fn), points_to_of(fn),
                                              ins, &native_decls_);
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
CallInfo callees_of(const ir::IrFunction &fn, const NativeDecls &decls) {
    CallInfo ci;
    for (const ir::IrBlock &b : fn.blocks)
        for (const ir::IrInstr &in : b.instrs) {
            switch (in.op) {
            case ir::IrOp::CALL:
            case ir::IrOp::TAILCALL:
                if (!in.func_name.empty()) ci.static_callees.push_back(in.func_name);
                else ci.dynamic = true;
                break;
            /* Una llamada NATIVA con nombre se resuelve como cualquier otra: si
             * la funcion esta en el programa, se ANALIZA -- no se dan por
             * supuestas sus capacidades ni se espera a que alguien las declare.
             * Solo cuando el destino no aparece (codigo verdaderamente ajeno)
             * el cierre sube al efecto maximo, que es donde ya lo hace.
             *
             * Antes se marcaba dinamica SIEMPRE, asi que aunque el destino
             * estuviera delante, su resumen no se miraba nunca. */
            case ir::IrOp::CALLN:
                /* Si alguien DIJO lo que hace, ya esta contado: el efecto
                 * declarado se aplico en el sitio de llamada, con su memoria
                 * resuelta.  Anadirla como callee ausente la volveria a subir
                 * al efecto maximo y la declaracion no habria servido de nada. */
                if (decls.count(in.func_name)) break;
                if (!in.func_name.empty())
                    ci.static_callees.push_back(in.func_name);
                else
                    ci.dynamic = true;
                break;
            case ir::IrOp::CALLVIRT:
            case ir::IrOp::CALLM:
            case ir::IrOp::CALLITF:
            case ir::IrOp::CALLCLOSURE:
            case ir::IrOp::CALLIND:
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

// Filtra de un LocSet las Stack locs cuya raiz NO escapa (scratch LOCAL, no
// observable por el caller).  El resto (Stack escapante, Heap, Global,
// ArgDerived, Unknown/top) se conserva.
LocSet filter_local_stack(const LocSet &s, const analysis::EscapeInfo &esc) {
    if (s.is_top) return s;
    LocSet out;
    for (const AbstractLoc &l : s.locs) {
        if (l.kind == AbstractLoc::Kind::Stack && l.id != LOC_GENERIC &&
            !esc.stack_escapes(l.id))
            continue; // scratch local -> no observable
        out.add(l);
    }
    return out;
}
// Aplica el filtro a reads + writes de un SemanticEffects (el efecto OBSERVABLE
// por el caller).  may_*/control/tags/determinism se conservan.
SemanticEffects observable_effect(SemanticEffects e,
                                  const analysis::EscapeInfo &esc) {
    e.mem.reads = filter_local_stack(e.mem.reads, esc);
    e.mem.writes = filter_local_stack(e.mem.writes, esc);
    return e;
}
} // namespace

ModuleSummary EffectAnalysis::build_summary(
    const std::vector<const ir::IrModule *> &mods) {
    ModuleSummary out;

    // Recorre TODAS las funciones de TODOS los modulos (interproc cross-modulo).
    auto for_each_fn = [&](auto &&f) {
        for (const ir::IrModule *m : mods)
            if (m)
                for (const ir::IrFunction &fn : m->functions) f(fn);
    };

    // 0) EscapeAnalysis del programa: que Stack roots locales escapan (sus
    //    escrituras SI son observables por el caller).  Provee la base de hechos
    //    via el manager (facts_of/points_to_of), no la construye aqui.  Para
    //    varios modulos se computa por-modulo (un callee de otro modulo se trata
    //    como externo = captura, sound).
    std::unordered_map<std::string, analysis::EscapeInfo> escape_all;
    {
        auto facts_fn = [this](const ir::IrFunction &fn) -> const IrFacts & {
            return facts_of(fn);
        };
        auto pt_fn = [this](const ir::IrFunction &fn) -> const analysis::PointsTo & {
            return points_to_of(fn);
        };
        for (const ir::IrModule *m : mods) {
            if (!m) continue;
            auto em = analysis::compute_escape_module(*m, facts_fn, pt_fn);
            for (auto &kv : em) escape_all[kv.first] = std::move(kv.second);
        }
    }

    // 1) Summary LOCAL de cada funcion (efecto propio, estructura) + lagunas.
    //    Antes se recogen las declaraciones de nativas: son parte de la entrada
    //    del analisis local (una nativa declarada aporta su efecto exacto ahi
    //    mismo, no una laguna).
    native_decls_ = collect_native_decls(mods);
    gaps_ = EffectGaps{};
    std::unordered_map<std::string, CallInfo> calls;
    // El efecto/completeness LOCAL se preserva aparte: el cierre (paso 2) se
    // recomputa SIEMPRE desde el local + callees (idempotente para el worklist).
    std::unordered_map<std::string, SemanticEffects>      local_eff;
    std::unordered_map<std::string, AnalysisCompleteness> local_comp;
    for_each_fn([&](const ir::IrFunction &fn) {
        EffectAnalysisResult loc = function_local_effects(fn, &gaps_, &native_decls_);
        FunctionSummary s;
        s.symbol = fn.name;
        s.semantic.local = loc.effects; // CRUDO (lo muestra --analyze "local")
        // El cierre parte del efecto OBSERVABLE: sin las escrituras/lecturas a
        // Stack scratch LOCAL (no las ve el caller) -> una reduccion que escribe
        // solo Stack#acc_slot pasa a readonly.
        SemanticEffects obs = observable_effect(loc.effects, escape_all[fn.name]);
        s.semantic.closure = obs;
        s.structural = structural_of(fn);
        s.completeness = loc.completeness;
        calls[fn.name] = callees_of(fn, native_decls_);
        s.interproc.reaches_dynamic_call = calls[fn.name].dynamic;
        s.interproc.has_calls =
            calls[fn.name].dynamic || !calls[fn.name].static_callees.empty();
        local_eff[fn.name] = obs; // OBSERVABLE (sin scratch local) = semilla del cierre
        local_comp[fn.name] = loc.completeness;
        out.fns.emplace(fn.name, std::move(s));
    });

    // 2) Punto-fijo EFICIENTE por WORKLIST (dataflow interprocedural clasico):
    //    closure(fn) = local(fn) U closure(callee) para cada callee.  Solo se
    //    re-procesa una funcion cuando el cierre de ALGUN callee suyo cambia
    //    -> O(aristas del callgraph x altura del reticulo), NO O(n^2).  Un
    //    callee ausente del mapa (externo al PROGRAMA / dinamico / nativo) hace
    //    el cierre CONSERVADOR (TOP robusto).  Con varios modulos, un callee de
    //    otro modulo SI esta en el mapa -> se resuelve (interproc cross-modulo).
    // Reverse-callgraph: callee -> callers (para re-encolar dependientes).
    std::unordered_map<std::string, std::vector<std::string>> callers;
    for (const auto &kv : calls)
        for (const std::string &callee : kv.second.static_callees)
            callers[callee].push_back(kv.first);

    // Recomputa el cierre de una funcion desde su local + los cierres de callees.
    // Devuelve true si cambio (para propagar a sus callers).
    auto recompute = [&](const std::string &name) -> bool {
        FunctionSummary &s = out.fns[name];
        SemanticEffects nc = local_eff[name];
        AnalysisCompleteness comp = local_comp[name];
        const CallInfo &ci = calls[name];
        auto raise = [&] {
            if (comp == AnalysisCompleteness::Complete)
                comp = AnalysisCompleteness::Conservative;
        };
        if (ci.dynamic) { nc = join(nc, SemanticEffects::top()); raise(); }
        for (const std::string &callee : ci.static_callees) {
            auto it = out.fns.find(callee);
            if (it == out.fns.end()) {
                /* El destino NO esta en el programa: es codigo ajeno de verdad
                 * y no hay nada que analizar, asi que efecto maximo.  Se apunta
                 * su NOMBRE, que es lo unico que permite hacer algo al
                 * respecto -- traerlo al analisis o declarar sus efectos. */
                nc = join(nc, SemanticEffects::top());
                raise();
                continue;
            }
            merge_callee(nc, it->second.semantic.closure);
            if (uint8_t(it->second.completeness) > uint8_t(comp))
                comp = it->second.completeness;
        }
        if (!(nc == s.semantic.closure) || comp != s.completeness) {
            s.semantic.closure = nc;
            s.completeness = comp;
            return true;
        }
        return false;
    };

    std::deque<std::string>         work;
    std::unordered_set<std::string> in_work;
    for (const auto &kv : out.fns) {
        work.push_back(kv.first);
        in_work.insert(kv.first);
    }
    while (!work.empty()) {
        std::string name = std::move(work.front());
        work.pop_front();
        in_work.erase(name);
        if (recompute(name)) {
            // El cierre de 'name' cambio -> sus callers pueden cambiar.
            auto cit = callers.find(name);
            if (cit != callers.end())
                for (const std::string &caller : cit->second)
                    if (in_work.insert(caller).second) work.push_back(caller);
        }
    }

    /* Ya convergido, se apunta UNA vez cada llamada a codigo que no esta en el
     * programa.  Hacerlo dentro del punto fijo contaba la misma llamada tantas
     * veces como vueltas diera. */
    for (const auto &kv : calls)
        for (const std::string &callee : kv.second.static_callees)
            if (out.fns.find(callee) == out.fns.end())
                gaps_.record_nativa(callee);
    return out;
}

const ModuleSummary &EffectAnalysis::module_summary(const ir::IrModule &mod) {
    if (!module_dirty_) return module_cache_;
    module_cache_ = build_summary({&mod});
    module_dirty_ = false;
    return module_cache_;
}

const ModuleSummary &EffectAnalysis::program_summary(
    const std::vector<const ir::IrModule *> &mods) {
    // Interprocedural a nivel de PROGRAMA (varios modulos).  No se cachea por
    // dirty (se recomputa: se invoca puntualmente para el analisis whole-program).
    program_cache_ = build_summary(mods);
    return program_cache_;
}

void EffectAnalysis::invalidate_node(const ir::IrFunction &fn,
                                     const ir::IrInstr &ins) {
    local_cache_.erase(static_cast<const void *>(&ins));
    invalidate_function(fn.name);
}

void EffectAnalysis::invalidate_function(const std::string &fn_name) {
    dirty_[fn_name] = true;
    module_dirty_ = true;
    // Los hechos (def-use/CFG) de la funcion cambiaron -> invalidar en el manager
    // para que se recomputen la proxima vez que se pidan.
    facts_mgr_.invalidate<IRFactsAnalysis>(fn_name);
    // TODO: propagar a los callers transitivos por el callgraph (SCC) cuando el
    // cierre interprocedural se cachee por-funcion (hoy module_summary lo rehace).
}

void EffectAnalysis::clear() {
    local_cache_.clear();
    summary_cache_.clear();
    dirty_.clear();
    module_cache_.fns.clear();
    facts_mgr_.clear(); // invalida los hechos cacheados
    module_dirty_ = true;
}

} // namespace effects
} // namespace analysis
