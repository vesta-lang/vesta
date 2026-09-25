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
#include "util/env_flags.h"
#include "analysis/effects/effect_analysis.h"

#include "analysis/facts/value_range.h"

#include "ir/ssa_ir.h"
#include "analysis/effects/ir_effects.h"
#include "analysis/escape/escape.h"
#include "aot/aot_analyze.h" // que necesita cada op para correr (backend AOT)

#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string_view>
#include <utility>

namespace analysis {
namespace effects {

const IrFacts &EffectAnalysis::facts_of(const ir::IrFunction &fn) {
    // Hechos fundacionales cacheados por el AnalysisManager: se computan una
    // vez por funcion y se reusan (antes se reconstruian en cada consulta local
    // -> O(n^2) por funcion; ahora O(n)).
    return facts_mgr_.get_or_compute<IRFactsAnalysis, IrFacts>(
        fn.name_key(), [&]() { return build_ir_facts(fn); });
}

const RangeFacts &EffectAnalysis::ranges_of(const ir::IrFunction &fn) {
    /* La MISMA instancia que la de por punto, quedandose con su mitad.
     *
     * Un productor, dos consumidores: points-to pregunta por valor y el
     * comprobador de limites por punto.  Calcularlos aparte serian dos puntos
     * fijos por funcion, y lo caro es el punto fijo -- no el estado por bloque
     * que uno de los dos no mira --.  Medido al separarlos: 500 analisis
     * pasaban a 600.
     *
     * Con los resumenes si alguien los dio, que es lo que hace que los dos
     * juzguen sobre exactamente la misma informacion. */
    /* UN productor, DOS consumidores: points-to pregunta por valor y el
     * comprobador de limites por punto, y los dos salen de este mismo
     * resultado.  El estado por bloque que necesita el segundo NO se paga
     * aparte: el punto fijo ya lo lleva y sacarlo es moverlo.
     *
     * Se probo separarlos en dos analisis para que el primero no cargara con
     * el estado, y salia peor -- 500 analisis pasaban a 600 --: lo caro es el
     * punto fijo, no el estado. */
    return *facts_mgr_.get_or_compute<
        RangeAnalysis, std::shared_ptr<const RangeFacts>>(fn.name_key(), [&]() {
        const RangeRequester mark(RangeAsker::Effects);
        return compute_ranges_ptr(fn, facts_of(fn), RangeOptions{}, resumenes_);
    });
}

const PointsTo &EffectAnalysis::points_to_of(const ir::IrFunction &fn) {
    // Tabla points-to cacheada; su factory consume facts_of (via el manager),
    // registrando la dependencia PointsTo -> IRFacts para la invalidacion.
    /* Con los RANGOS: un desplazamiento variable deja de ser "en algun sitio"
     * y pasa a ser un intervalo.  Se piden por el mismo gestor, asi que se
     * calculan una vez por funcion y los comparte quien los necesite. */
    return facts_mgr_.get_or_compute<PointsToAnalysis, PointsTo>(
        fn.name_key(), [&]() {
            const IrFacts &f = facts_of(fn);
            const RangeFacts &rg = ranges_of(fn);
            return compute_points_to(fn, f, &rg);
        });
}

EffectAnalysisResult EffectAnalysis::local(const ir::IrFunction &fn,
                                           const ir::IrInstr &ins) {
    /* Se deriva, no se memoriza.  El porque -- y la medida -- estan en la
     * cabecera: el memo que habia aqui iba con la DIRECCION de la instruccion
     * por clave, o sea con su posicion, y ademas nunca acertaba. */
    return effects_of_instr(fn, facts_of(fn), points_to_of(fn), ins, env_);
}

namespace {

/**
 * @brief El indice de la funcion que implementa @p name en @p ms.
 *
 * Primero por el nombre completo y, si no esta, por lo que va detras del
 * ultimo `:`.  El segundo no es un apano: una nativa se nombra "lib:fn", y es
 * como se llama la funcion cuando la implementacion resulta ser codigo del
 * lenguaje -- el usuario redefine una primitiva en Vesta, o en nativo el
 * runtime de I/O se trae de `vx_io.vx` --.
 *
 * @return @ref NO_SUMMARY_FN si no esta: entonces si es codigo ajeno.
 */
SummaryFnIdx resolve_callee(const ModuleSummary &ms, std::string_view name) {
    const SummaryFnIdx i = ms.find(name);
    if (i != NO_SUMMARY_FN) return i;
    const size_t sep = name.rfind(':');
    if (sep == std::string_view::npos || sep + 1 >= name.size())
        return NO_SUMMARY_FN;
    return ms.find(name.substr(sep + 1));
}

/// El resumen de @p name segun @ref resolve_callee, o nulo si no esta.
const FunctionSummary *find_callee(const ModuleSummary &ms,
                                   std::string_view name) {
    const SummaryFnIdx i = resolve_callee(ms, name);
    return i == NO_SUMMARY_FN ? nullptr : &ms.fns[i];
}

} // namespace

EfectoEnLlamada EffectAnalysis::at_call_site(const ir::IrFunction &caller,
                                             const ir::IrInstr &call) {
    EfectoEnLlamada r;
    if (call.op != ir::IrOp::CALL && call.op != ir::IrOp::TAILCALL &&
        call.op != ir::IrOp::CALLN) {
        r.completo = false;
        return r;
    }
    if (call.func_name.empty()) {
        r.completo = false;
        return r;
    }

    /* El cierre del destino.  Se busca en lo que ya se calculo: primero el
     * programa (varios modulos) y si no el modulo.  Sin resumen no hay nada que
     * instanciar -- una funcion de fuera del programa no dice lo que hace --, y
     * eso lo trata quien pregunta como lo que es: falta de conocimiento, no
     * ausencia de efecto. */
    // Una nativa se nombra "lib:fn" y tambien "fn" a secas.
    const FunctionSummary *s = find_callee(program_cache_, call.func_name);
    if (s == nullptr) s = find_callee(module_cache_, call.func_name);
    if (s == nullptr) {
        r.completo = false; // no hay resumen: no se sabe lo que hace.
        return r;
    }

    /* Se traduce el CIERRE: lo que la funcion hace por si misma y lo que hacen
     * las que llama.
     *
     * Se puede porque el cierre ya viene en terminos de los parametros de ESTA
     * funcion: al construirlo, lo que aporta cada callee se traduce con los
     * argumentos de su sitio de llamada.  Antes no era asi -- se copiaban los
     * `arg#N` del callee tal cual, que son los suyos, no los de aqui -- y por
     * eso esto miraba solo el efecto propio: lo unico entonces interpretable.
     *
     * Con el cierre, lo que hace una funcion tres niveles mas abajo llega hasta
     * el sitio donde se puede juzgar. */
    r = instanciar_en_llamada(s->semantic.closure, call.operands,
                              points_to_of(caller));
    /* Un resumen que ya venia incompleto no se vuelve completo por traducirlo:
     * lo que no se supo alli sigue sin saberse aqui. */
    if (s->completeness != AnalysisCompleteness::Complete) r.completo = false;
    return r;
}

// Extrae la faceta ESTRUCTURAL de una funcion (bloques, back-edges = bucles,
// recursion directa).  El detalle de trip-counts (para Big-O) lo afina el
// subsistema de coste; aqui damos la forma.
/// Si saltar de @p from a @p to vuelve atras en el orden de bloques.
static inline bool is_back_edge(ir::IrBlockId from, ir::IrBlockId to) {
    return to != ir::IR_NO_BLOCK && to <= from;
}

static StructuralSummary structural_of(const ir::IrFunction &fn) {
    StructuralSummary st;
    st.block_count = static_cast<uint32_t>(fn.blocks.size());
    // Back-edge = arista a un bloque de indice <= el actual (aproximacion de
    // bucle sobre el orden de bloques).  Recursion directa = se llama a si
    // misma.
    for (ir::IrBlockId bi = ir::IrBlockId(0); bi < fn.blocks.size(); ++bi) {
        for (const ir::IrInstr &in : fn.blocks[bi].instrs) {
            if (in.op == ir::IrOp::BR) {
                if (is_back_edge(bi, in.target_block)) ++st.loop_count;
            } else if (in.op == ir::IrOp::BR_COND) {
                if (is_back_edge(bi, in.target_block) ||
                    is_back_edge(bi, in.false_block))
                    ++st.loop_count;
            } else if (in.op == ir::IrOp::SWITCH_DENSE ||
                       in.op == ir::IrOp::MATCH_VARIANT) {
                for (ir::IrBlockId t : in.jump_targets)
                    if (is_back_edge(bi, t)) {
                        ++st.loop_count;
                        break;
                    }
            }
            if ((in.op == ir::IrOp::CALL || in.op == ir::IrOp::TAILCALL) &&
                in.func_name == fn.name)
                st.recursive = true;
        }
    }
    if (st.loop_count > 0)
        st.has_unbounded_loop = true; // conservador sin trip-count
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
    s.semantic.closure =
        loc.effects; // sin interproc; module_summary lo completa
    s.structural = structural_of(fn);
    s.completeness = loc.completeness;
    return s;
}

const FunctionSummary &EffectAnalysis::summary(const ir::IrModule &mod,
                                               const ir::IrFunction &fn) {
    const std::string *key = fn.name_key();
    auto d = dirty_.find(key);
    const bool is_dirty = (d == dirty_.end()) || d->second;
    auto it = summary_cache_.find(key);
    if (!is_dirty && it != summary_cache_.end()) return it->second;
    FunctionSummary s = compute_summary(mod, fn);
    dirty_[key] = false;
    auto res = summary_cache_.insert_or_assign(key, std::move(s));
    return res.first->second;
}

// Callees de una funcion: nombres estaticos (CALL/TAILCALL) + si hace alguna
// llamada DINAMICA/nativa (callee desconocido -> el cierre toma el efecto TOP
// robusto: puede hacer cualquier cosa, con el motivo registrado para el
// reporte).
namespace {
struct CallInfo {
    /**
     * @brief Los SITIOS de llamada, con sus argumentos.
     *
     * El cierre se calcula por NOMBRE, y por nombre no se puede traducir lo que
     * el llamado dice de sus parametros: `arg#1` de el no es `arg#1` de quien
     * llama.  Con los argumentos de CADA sitio si -- y es lo que hace que lo
     * que toca una funcion de tres niveles abajo llegue arriba hablando de la
     * memoria de aqui, en vez de perderse en "toca algo".
     */
    struct Site {
        /* La llamada misma: de ella salen el nombre y los argumentos.  Sin
         * copiar nada -- el IR vive mientras se construye el resumen --. */
        const ir::IrInstr *call = nullptr;
        /// A quien llama, resuelto UNA vez.  @ref NO_SUMMARY_FN = no esta en
        /// el programa: codigo ajeno de verdad.
        SummaryFnIdx target = NO_SUMMARY_FN;
    };
    /// Llamadas estaticas (CALL/TAILCALL) con nombre.
    std::vector<Site> sites;
    /// La funcion donde estan esos sitios, para resolver sus argumentos.
    const ir::IrFunction *fn = nullptr;

    bool dynamic = false; // CALLVIRT/CALLIND/CALLN/...
    /// En AOT, una op que depende del runtime ES una llamada a libvesta_rt.
    /// No es un callee desconocido -- el helper hace exactamente esa op, y su
    /// efecto ya esta modelado --, pero la funcion deja de ser hoja.
    bool runtime = false;
    /// Llamadas NATIVAS por su nombre completo "lib:fn".  Van aparte porque
    /// resuelven en DOS pasos (ver el cierre): el nombre completo y, si no
    /// esta, el simbolo a secas -- que es como acaba llamandose cuando la
    /// implementacion resulta ser codigo del propio lenguaje.
    std::vector<Site> natives;
};
CallInfo callees_of(const ir::IrFunction &fn, const EffectEnv &env) {
    CallInfo ci;
    ci.fn = &fn;
    const NativeDecls *decls = env.decls;
    for (const ir::IrBlock &b : fn.blocks)
        for (const ir::IrInstr &in : b.instrs) {
            if (env.backend == Backend::Aot &&
                ::aot::aot_classify_op(in.op) ==
                    ::aot::AotOpClass::RUNTIME_DEPENDENT)
                ci.runtime = true;
            switch (in.op) {
            case ir::IrOp::CALL:
            case ir::IrOp::TAILCALL:
                if (!in.func_name.empty()) {
                    ci.sites.push_back(CallInfo::Site{&in, NO_SUMMARY_FN});
                } else {
                    ci.dynamic = true;
                }
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
                 * al efecto maximo y la declaracion no habria servido de nada.
                 */
                if (decls && decls->count(in.func_name)) break;
                if (!in.func_name.empty())
                    ci.natives.push_back(CallInfo::Site{&in, NO_SUMMARY_FN});
                else
                    ci.dynamic = true;
                break;
            case ir::IrOp::CALLVIRT:
            case ir::IrOp::CALLM:
            case ir::IrOp::CALLITF:
            case ir::IrOp::CALLCLOSURE:
            case ir::IrOp::CALLIND: ci.dynamic = true; break;
            default: break;
            }
        }
    return ci;
}

/**
 * @brief Une el cierre del callee en el del caller.
 *
 * Lo que el callee dice de SUS parametros no significa nada aqui: `arg#1` es el
 * primero de EL, no del que llama, y copiarlo tal cual hace que una funcion de
 * tres parametros acabe afirmando que escribe en su `arg#6`.  No es impreciso,
 * es falso -- y se estaba imprimiendo en el informe.
 *
 * Traducirlo requiere los ARGUMENTOS, que solo existen en el sitio de llamada;
 * este cierre se calcula por NOMBRE, asi que aqui lo unico honesto es decir que
 * toca memoria sin saber cual.  Quien quiera la version traducida la pide donde
 * se puede dar: @ref EffectAnalysis::at_call_site.
 */
/// Cambia lo que @p s dice de los parametros del callee por "algo, no se que":
/// sus parametros no se pueden nombrar desde el llamante.
void depersonalize(LocSet &s) {
    if (s.is_top) return;
    bool has_arg = false;
    for (const AbstractLoc &l : s.locs)
        if (l.kind == AbstractLoc::Kind::ArgDerived) has_arg = true;
    if (!has_arg) return;
    LocSet out;
    for (const AbstractLoc &l : s.locs) {
        if (l.kind == AbstractLoc::Kind::ArgDerived) {
            out.add(AbstractLoc{AbstractLoc::Kind::Unknown, LOC_GENERIC});
            continue;
        }
        out.add(l);
    }
    s = std::move(out);
}

void merge_callee(SemanticEffects &caller, const SemanticEffects &callee) {
    SemanticEffects c = callee;
    depersonalize(c.mem.reads);
    depersonalize(c.mem.writes);
    caller = join(caller, c);
}

/**
 * @brief Colapsa las posiciones de @p s a "la raiz entera" si pasan del tope.
 *
 * Traducir en cada sitio hace que una funcion recursiva sobre punteros genere
 * una posicion nueva por vuelta -- `f(p+8)` da p+0, p+8, p+16... -- y el punto
 * fijo dejaria de terminar.  Pasado el tope se pierde precision, no correccion,
 * y el calculo termina.  Es el limite DECLARADO que cualquier analisis con
 * punto fijo necesita.
 */
void cap_locs(LocSet &s) {
    constexpr size_t kLocCap = 64;
    if (s.is_top || s.locs.size() <= kLocCap) return;
    LocSet out;
    for (const AbstractLoc &l : s.locs)
        out.add(AbstractLoc{l.kind, l.id, 0, 0}); // toda la raiz
    s = std::move(out);
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

/* Fuera del espacio anonimo: `scratch` ya existe en este espacio (lo abre el
 * resumen), y dos con el mismo nombre harian ambigua cada etiqueta. */
namespace scratch {
struct SummaryCalls;         ///< Las llamadas de cada funcion.
struct SummaryLocalEffects;  ///< El efecto observable propio de cada funcion.
struct SummaryLocalComp;     ///< Lo completo que es ese efecto propio.
struct SummaryCallerOffsets; ///< Funcion -> donde empiezan sus llamantes.
struct SummaryCallers;       ///< Los llamantes de cada funcion.
struct SummaryQueued;        ///< Si cada funcion esta ya en la cola.
} // namespace scratch

namespace {

/// Posicion dentro de la lista de llamantes.
enum SummaryEdge : uint32_t {};
/// Si una funcion espera ya en la cola del punto fijo.
enum class SummaryQueueState : uint8_t { IDLE, QUEUED };

/// Reloj de las fases del resumen, para el reparto de VESTA_TIMES.
struct SummaryPhaseClock {
    std::chrono::steady_clock::time_point mark =
        std::chrono::steady_clock::now();
    /// Suma a @p dst lo que ha pasado desde la ultima marca, y marca.
    void close(long &dst) {
        const auto now = std::chrono::steady_clock::now();
        dst += static_cast<long>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - mark)
                .count());
        mark = now;
    }
};

/// Los hechos de una funcion, pedidos al motor (para el escape).
struct EngineFactsOf {
    EffectAnalysis &engine;
    const IrFacts &operator()(const ir::IrFunction &fn) const {
        return engine.facts_publico(fn);
    }
};

/// La tabla points-to de una funcion, pedida al motor (para el escape).
struct EnginePointsToOf {
    EffectAnalysis &engine;
    const analysis::PointsTo &operator()(const ir::IrFunction &fn) const {
        return engine.points_to_publico(fn);
    }
};

/// Sube @p comp a conservador: algo no se supo.
inline void raise_to_conservative(AnalysisCompleteness &comp) {
    if (comp == AnalysisCompleteness::Complete)
        comp = AnalysisCompleteness::Conservative;
}

/// @p comp pasa a ser lo peor entre el y @p other.
inline void worsen(AnalysisCompleteness &comp, AnalysisCompleteness other) {
    if (uint8_t(other) > uint8_t(comp)) comp = other;
}

/**
 * @brief Un paso del punto fijo: recalcula el cierre de UNA funcion desde su
 *        efecto propio y los cierres de lo que llama.
 *
 * Todo por indice: los destinos de cada llamada se resolvieron una vez antes
 * de empezar, asi que aqui no se mira ningun nombre.
 */
struct ClosureStep {
    ModuleSummary &out;
    const util::NamedVector<CallInfo, scratch::SummaryCalls> &calls;
    const util::NamedVector<SemanticEffects, scratch::SummaryLocalEffects>
        &local_eff;
    const util::NamedVector<AnalysisCompleteness, scratch::SummaryLocalComp>
        &local_comp;
    EffectAnalysis &engine;

    /// @return true si el cierre de @p i cambio (hay que avisar a quien la
    ///         llama).
    bool operator()(SummaryFnIdx i) const {
        SemanticEffects nc = local_eff[i];
        AnalysisCompleteness comp = local_comp[i];
        const CallInfo &ci = calls[i];
        if (ci.dynamic) {
            nc = join(nc, SemanticEffects::top());
            raise_to_conservative(comp);
        }
        /* Lo que hace cada llamada, TRADUCIDO a la memoria de aqui.
         *
         * `arg#1` del llamado no es `arg#1` de quien llama.  Con los
         * argumentos del SITIO si, y eso es lo que hace que un `memcpy` que
         * reparte a una variante interna siga diciendo, tres niveles mas
         * arriba, que escribe en lo que le pasaron -- en vez de perderse en
         * "toca algo".
         *
         * Lo que no se pueda traducir (la pila del llamado, su monton) sube a
         * desconocido, que es lo unico cierto: aqui esos nombres no significan
         * nada. */
        for (const CallInfo::Site &site : ci.sites) {
            if (site.target == NO_SUMMARY_FN) {
                /* El destino NO esta en el programa: es codigo ajeno de verdad
                 * y no hay nada que analizar, asi que efecto maximo. */
                nc = join(nc, SemanticEffects::top());
                raise_to_conservative(comp);
                continue;
            }
            const FunctionSummary &cs = out.fns[site.target];
            const SemanticEffects &ce = cs.semantic.closure;
            if (ci.fn != nullptr) {
                const EfectoEnLlamada e = instanciar_en_llamada(
                    ce, site.call->operands, engine.points_to_publico(*ci.fn));
                SemanticEffects trad = ce;
                trad.mem.reads = e.lee;
                trad.mem.writes = e.escribe;
                if (!e.completo) {
                    // Habia algo que no se pudo nombrar: se dice, sin tirar lo
                    // demas por el camino equivocado -- el join lo absorbera si
                    // hace falta.
                    trad.mem.reads.add(
                        AbstractLoc{AbstractLoc::Kind::Unknown, LOC_GENERIC});
                    trad.mem.writes.add(
                        AbstractLoc{AbstractLoc::Kind::Unknown, LOC_GENERIC});
                }
                nc = join(nc, trad);
            } else {
                merge_callee(nc, ce);
            }
            worsen(comp, cs.completeness);
        }
        /* Las nativas, ya resueltas en sus dos pasos ("lib:fn" y "fn" a
         * secas; ver @ref resolve_callee).  Si la implementacion esta delante
         * hay que analizarla, no darla por ajena. */
        for (const CallInfo::Site &site : ci.natives) {
            if (site.target == NO_SUMMARY_FN) {
                nc = join(nc, SemanticEffects::top());
                raise_to_conservative(comp);
                continue;
            }
            const FunctionSummary &cs = out.fns[site.target];
            merge_callee(nc, cs.semantic.closure);
            worsen(comp, cs.completeness);
        }
        cap_locs(nc.mem.reads);
        cap_locs(nc.mem.writes);
        FunctionSummary &s = out.fns[i];
        if (!(nc == s.semantic.closure) || comp != s.completeness) {
            s.semantic.closure = std::move(nc);
            s.completeness = comp;
            return true;
        }
        return false;
    }
};

} // namespace

ModuleSummary
EffectAnalysis::build_summary(const std::vector<const ir::IrModule *> &mods) {
    ModuleSummary out;

    /* Reparto del coste, por fases.  Se publica con VESTA_TIMES porque "el
     * analisis tarda" no se puede atacar sin saber cual de las cuatro es. */
    const bool medir = util::flag_on(util::FlagId::Times);
    SummaryPhaseClock clock;
    long us_escape = 0, us_local = 0, us_punto_fijo = 0, us_nativas = 0;
    long n_fns = 0, n_pasos = 0;

    // 0) EscapeAnalysis del programa: que Stack roots locales escapan (sus
    //    escrituras SI son observables por el caller).  Provee la base de
    //    hechos via el manager (facts_of/points_to_of), no la construye aqui.
    //    Para varios modulos se computa por-modulo (un callee de otro modulo se
    //    trata como externo = captura, sound).  Uno por modulo, en el orden de
    //    @p mods, y dentro por la posicion de cada funcion.
    std::vector<analysis::ModuleEscape> escapes(mods.size());
    for (size_t m = 0; m < mods.size(); ++m)
        if (mods[m])
            escapes[m] = analysis::compute_escape_module(
                *mods[m], EngineFactsOf{*this}, EnginePointsToOf{*this});
    clock.close(us_escape);

    // 1) Summary LOCAL de cada funcion (efecto propio, estructura) + lagunas.
    //    Antes se recogen las declaraciones de nativas: son parte de la entrada
    //    del analisis local (una nativa declarada aporta su efecto exacto ahi
    //    mismo, no una laguna).
    native_decls_ = collect_native_decls(mods);
    env_.decls = &native_decls_;
    gaps_ = EffectGaps{};
    /* Todo lo de cada funcion, por su INDICE en `out`. */
    util::NamedVector<CallInfo, scratch::SummaryCalls> calls;
    // El efecto/completeness LOCAL se preserva aparte: el cierre (paso 2) se
    // recomputa SIEMPRE desde el local + callees (idempotente para el
    // worklist).
    util::NamedVector<SemanticEffects, scratch::SummaryLocalEffects> local_eff;
    util::NamedVector<AnalysisCompleteness, scratch::SummaryLocalComp>
        local_comp;
    // Recorre TODAS las funciones de TODOS los modulos (interproc
    // cross-modulo).
    for (size_t m = 0; m < mods.size(); ++m) {
        if (!mods[m]) continue;
        for (size_t pos = 0; pos < mods[m]->functions.size(); ++pos) {
            const ir::IrFunction &fn = mods[m]->functions[pos];
            /* Un nombre repetido entre modulos: vale el primero, entero.
             * Antes el resumen era el del primero y las llamadas las del
             * ultimo. */
            if (out.find(fn.name) != NO_SUMMARY_FN) continue;
            /* Los rangos de la funcion, UNA vez.  Sin esto, cada bloque de asm
             * los recalcula recorriendo la funcion entera desde dentro del
             * modelo de efectos.  Salen de la cache si algun otro consumidor
             * ya los pidio.  Solo si hay asm: es lo unico que los mira. */
            if (funcion_tiene_asm(fn)) {
                env_.rangos = &ranges_of(fn); // el accesor cacheado
                env_.rangos_de = &fn;
            }
            /* Se retiran al salir: prestados a la SIGUIENTE funcion serian una
             * respuesta incorrecta en silencio. */
            struct Retirar {
                EffectEnv &e;
                ~Retirar() {
                    e.rangos = nullptr;
                    e.rangos_de = nullptr;
                }
            } retirar{env_};
            EffectAnalysisResult loc =
                function_local_effects(fn, &gaps_, env_);
            FunctionSummary s;
            s.symbol = fn.name;
            s.semantic.local = loc.effects; // CRUDO (lo muestra --analyze)
            // El cierre parte del efecto OBSERVABLE: sin las escrituras/
            // lecturas a Stack scratch LOCAL (no las ve el caller) -> una
            // reduccion que escribe solo Stack#acc_slot pasa a readonly.
            SemanticEffects obs =
                observable_effect(loc.effects, escapes[m].by_fn[pos]);
            s.semantic.closure = obs;
            s.structural = structural_of(fn);
            s.completeness = loc.completeness;
            CallInfo ci = callees_of(fn, env_);
            s.interproc.reaches_dynamic_call = ci.dynamic;
            s.interproc.has_calls =
                ci.dynamic || ci.runtime || !ci.sites.empty();
            out.add(fn.name_key(), std::move(s));
            calls.push_back(std::move(ci));
            // OBSERVABLE (sin scratch local) = semilla del cierre.
            local_eff.push_back(std::move(obs));
            local_comp.push_back(loc.completeness);
            ++n_fns;
        }
    }
    const size_t n = out.fns.size();

    /* A quien llama cada sitio, resuelto UNA vez ahora que estan todas.  Una
     * llamada estatica por su nombre exacto; una nativa en sus dos pasos. */
    for (CallInfo &ci : calls) {
        for (CallInfo::Site &site : ci.sites)
            site.target = out.find(site.call->func_name);
        for (CallInfo::Site &site : ci.natives)
            site.target = resolve_callee(out, site.call->func_name);
    }
    clock.close(us_local);

    // 2) Punto-fijo EFICIENTE por WORKLIST (dataflow interprocedural clasico):
    //    closure(fn) = local(fn) U closure(callee) para cada callee.  Solo se
    //    re-procesa una funcion cuando el cierre de ALGUN callee suyo cambia
    //    -> O(aristas del callgraph x altura del reticulo), NO O(n^2).  Un
    //    callee ausente del mapa (externo al PROGRAMA / dinamico / nativo) hace
    //    el cierre CONSERVADOR (TOP robusto).  Con varios modulos, un callee de
    //    otro modulo SI esta en el mapa -> se resuelve (interproc
    //    cross-modulo).
    /* Reverse-callgraph: callee -> callers (para re-encolar dependientes), en
     * plano: primero cuantos llamantes tiene cada una, luego el reparto.  Las
     * nativas cuentan igual: si su implementacion esta en el programa, cuando
     * su cierre cambie hay que volver a mirar a quien la llama. */
    util::NamedVector<SummaryEdge, scratch::SummaryCallerOffsets> caller_off(
        n + 1, SummaryEdge(0));
    for (size_t i = 0; i < n; ++i) {
        for (const CallInfo::Site &site : calls[i].sites)
            if (site.target != NO_SUMMARY_FN)
                caller_off[site.target + 1] =
                    SummaryEdge(caller_off[site.target + 1] + 1);
        for (const CallInfo::Site &site : calls[i].natives)
            if (site.target != NO_SUMMARY_FN)
                caller_off[site.target + 1] =
                    SummaryEdge(caller_off[site.target + 1] + 1);
    }
    for (size_t i = 0; i < n; ++i)
        caller_off[i + 1] = SummaryEdge(caller_off[i + 1] + caller_off[i]);
    util::NamedVector<SummaryFnIdx, scratch::SummaryCallers> callers(
        caller_off[n]);
    {
        util::NamedVector<SummaryEdge, scratch::SummaryCallerOffsets> next(
            caller_off.begin(), caller_off.end() - 1);
        for (size_t i = 0; i < n; ++i) {
            const SummaryFnIdx caller = static_cast<SummaryFnIdx>(i);
            for (const CallInfo::Site &site : calls[i].sites)
                if (site.target != NO_SUMMARY_FN) {
                    callers[next[site.target]] = caller;
                    next[site.target] = SummaryEdge(next[site.target] + 1);
                }
            for (const CallInfo::Site &site : calls[i].natives)
                if (site.target != NO_SUMMARY_FN) {
                    callers[next[site.target]] = caller;
                    next[site.target] = SummaryEdge(next[site.target] + 1);
                }
        }
    }

    /* La cola y la marca de "ya encolado" van por INDICE: encolar ya no copia
     * ni hashea ningun nombre, y la marca es una posicion en un vector. */
    const ClosureStep step{out, calls, local_eff, local_comp, *this};
    std::deque<SummaryFnIdx> work;
    util::NamedVector<SummaryQueueState, scratch::SummaryQueued> queued(
        n, SummaryQueueState::QUEUED);
    for (size_t i = 0; i < n; ++i)
        work.push_back(static_cast<SummaryFnIdx>(i));
    while (!work.empty()) {
        const SummaryFnIdx i = work.front();
        work.pop_front();
        queued[i] = SummaryQueueState::IDLE;
        ++n_pasos;
        if (!step(i)) continue;
        // El cierre de @c i cambio -> sus callers pueden cambiar.
        for (size_t e = caller_off[i]; e < caller_off[i + 1]; ++e) {
            const SummaryFnIdx caller = callers[e];
            if (queued[caller] == SummaryQueueState::QUEUED) continue;
            queued[caller] = SummaryQueueState::QUEUED;
            work.push_back(caller);
        }
    }

    clock.close(us_punto_fijo);

    /* Ya convergido, se apunta UNA vez cada llamada a codigo que no esta en el
     * programa.  Hacerlo dentro del punto fijo contaba la misma llamada tantas
     * veces como vueltas diera.  Se apunta su NOMBRE, que es lo unico que
     * permite hacer algo al respecto -- traerlo al analisis o declarar sus
     * efectos. */
    for (const CallInfo &ci : calls) {
        for (const CallInfo::Site &site : ci.sites)
            if (site.target == NO_SUMMARY_FN)
                gaps_.record_nativa(site.call->func_name);
        for (const CallInfo::Site &site : ci.natives)
            if (site.target == NO_SUMMARY_FN)
                gaps_.record_nativa(site.call->func_name);
    }
    clock.close(us_nativas);
    if (medir)
        std::cerr << "[efectos] " << n_fns << " funciones, " << n_pasos
                  << " pasos del punto fijo | escape " << us_escape
                  << " us | local " << us_local << " us | punto-fijo "
                  << us_punto_fijo << " us | nativas " << us_nativas << " us\n";
    return out;
}

const ModuleSummary &EffectAnalysis::module_summary(const ir::IrModule &mod) {
    if (!module_dirty_) return module_cache_;
    module_cache_ = build_summary({&mod});
    module_dirty_ = false;
    return module_cache_;
}

const ModuleSummary &
EffectAnalysis::program_summary(const std::vector<const ir::IrModule *> &mods) {
    // Interprocedural a nivel de PROGRAMA (varios modulos).  No se cachea por
    // dirty (se recomputa: se invoca puntualmente para el analisis
    // whole-program).
    program_cache_ = build_summary(mods);
    return program_cache_;
}

void EffectAnalysis::invalidate_node(const ir::IrFunction &fn,
                                     const ir::IrInstr &ins) {
    /* Del nodo ya no hay nada que borrar: su efecto local se deriva cuando se
     * pide.  Lo que si caduca es el resumen de la funcion, que si esta
     * cacheado. */
    (void)ins;
    invalidate_function(fn.name);
}

void EffectAnalysis::invalidate_function(const std::string &fn_name) {
    /* Se INTERNA aqui: la invalidacion llega con un nombre suelto, no con una
     * funcion.  Internar da el mismo puntero que uso quien lo guardo. */
    const std::string *key = util::intern_name(fn_name);
    dirty_[key] = true;
    module_dirty_ = true;
    // Los hechos (def-use/CFG) de la funcion cambiaron -> invalidar en el
    // manager para que se recomputen la proxima vez que se pidan.
    facts_mgr_.invalidate<IRFactsAnalysis>(key);
    // TODO: propagar a los callers transitivos por el callgraph (SCC) cuando el
    // cierre interprocedural se cachee por-funcion (hoy module_summary lo
    // rehace).
}

void EffectAnalysis::clear() {
    summary_cache_.clear();
    dirty_.clear();
    module_cache_.clear();
    facts_mgr_.clear(); // invalida los hechos cacheados
    module_dirty_ = true;
}

} // namespace effects
} // namespace analysis
