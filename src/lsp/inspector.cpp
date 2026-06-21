/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file inspector.cpp
 * @brief Implementacion del inspector del ecosistema Vex (peticiones
 *        @c vesta/* del LSP).
 *
 * Cada metodo es BAJO DEMANDA: se sirve cuando el editor lo pide, no en
 * cada pulsacion.  Las vistas baratas reutilizan el @c CompileResult
 * cacheado por @c AnalysisEngine; las caras (ir-pre, diagramas) recompilan
 * con flags concretos y cachean el resultado en @c view_cache_.  El codigo
 * nativo del JIT y del AOT se desensambla con Capstone (x86-64).
 *
 * Robustez: ningun metodo propaga excepciones; ante un fallo controlado
 * devuelve un objeto con @c error / @c unsupported / @c incompatible.  El
 * caller (lsp_server) ademas envuelve todo en try/catch como red final.
 */

#include "lsp/inspector.h"

#include <exception>
#include <sstream>

#include <capstone/capstone.h>

#include "aot/aot_analyze.h"
#include "analyze/bigo.h"
#include "ir/ssa_ir.h"
#include "ir/ssa_ir_serialize.h"
#include "jit/code_cache.h"
#include "jit/jit_compiler.h"
#include "jit/runtime_entries.h"
#include "jit/selector.h"
#include "jit/vreg_pipeline.h"
#include "lsp/document_store.h"
#include "vex/compiler.h"

namespace lsp {

namespace {

/**
 * @brief Devuelve la primera linea fuente conocida de una funcion IR.
 *
 * El IR no guarda una SourceLoc por funcion; recorre sus instrucciones
 * buscando el primer @c source_line distinto de cero.  Best-effort para
 * ubicar funciones en la lista de @c vesta/functions.
 *
 * @param fn Funcion IR.
 * @return Linea 1-based, o 0 si ninguna instruccion lleva linea.
 */
uint32_t first_source_line(const ir::IrFunction &fn) {
    for (const auto &blk : fn.blocks) {
        for (const auto &ins : blk.instrs) {
            if (ins.source_line != 0)
                return ins.source_line;
        }
    }
    return 0;
}

/**
 * @brief Deserializa el modulo IR (post-opt) cacheado del CompileResult.
 *
 * @param result CompileResult con @c ir_module_cache_bytes.
 * @param out    Modulo IR destino.
 * @return true si la deserializacion fue exitosa y el modulo tiene
 *         funciones; false en otro caso.
 */
bool parse_post_opt_module(const vex::CompileResult &result, ir::IrModule &out) {
    if (result.ir_module_cache_bytes.empty())
        return false;
    return ir::parse_ir_module_cache(result.ir_module_cache_bytes, out);
}

/**
 * @brief Selecciona la funcion objetivo de las vistas JIT/AOT.
 *
 * Si @p wanted no esta vacio, busca esa funcion exacta.  Si esta vacio,
 * prefiere @c "main"; si no hay, devuelve la primera funcion no nativa y
 * no macro-compilada del modulo.
 *
 * @param mod    Modulo IR.
 * @param wanted Nombre pedido (vacio = auto).
 * @return Puntero a la funcion, o nullptr si no hay candidata.
 */
const ir::IrFunction *pick_function(const ir::IrModule &mod,
                                    const std::string &wanted) {
    if (!wanted.empty()) {
        for (const auto &fn : mod.functions) {
            if (fn.name == wanted)
                return &fn;
        }
        return nullptr;
    }
    // Auto: preferir main (nombre exacto o sufijo "main").
    const ir::IrFunction *first_user = nullptr;
    for (const auto &fn : mod.functions) {
        if (fn.is_native || fn.is_macro_compiled)
            continue;
        if (!first_user)
            first_user = &fn;
        if (fn.name == "main")
            return &fn;
    }
    return first_user;
}

/**
 * @brief Desensambla un buffer de bytes x86-64 a texto legible via Capstone.
 *
 * Una linea por instruccion con offset relativo, mnemonico y operandos.  Si
 * Capstone no puede abrir o decodificar, devuelve un mensaje informativo en
 * lugar de fallar.
 *
 * @param code      Bytes del codigo nativo.
 * @param code_size Numero de bytes.
 * @param base      Direccion base mostrada (offset relativo si 0).
 * @return Texto del desensamblado (multilinea).
 */
std::string disasm_x86_64(const uint8_t *code, size_t code_size,
                          uint64_t base) {
    if (!code || code_size == 0)
        return "(codigo vacio)";
    csh handle;
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
        return "(Capstone: no se pudo abrir el desensamblador x86-64)";
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_OFF);
    cs_insn *insn = nullptr;
    const size_t count = cs_disasm(handle, code, code_size, base, 0, &insn);
    std::ostringstream oss;
    if (count > 0) {
        for (size_t i = 0; i < count; ++i) {
            // Offset relativo al inicio (mas estable que una direccion host).
            uint64_t off = insn[i].address - base;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%04llx",
                          static_cast<unsigned long long>(off));
            oss << buf << "  " << insn[i].mnemonic;
            if (insn[i].op_str[0] != '\0')
                oss << ' ' << insn[i].op_str;
            oss << '\n';
        }
        cs_free(insn, count);
    } else {
        oss << "(Capstone no pudo desensamblar los bytes generados)\n";
    }
    cs_close(&handle);
    return oss.str();
}

/**
 * @brief Mapea el nombre de tier textual al enum AOT.
 * @param tier "bare" | "embed" | "full" (insensible a otras formas).
 * @return Tier correspondiente (BARE por defecto).
 */
aot::Tier tier_from_str(const std::string &tier) {
    if (tier == "full")
        return aot::Tier::FULL;
    if (tier == "embed")
        return aot::Tier::EMBED;
    return aot::Tier::BARE;
}

/**
 * @brief Nombre legible de un kind de relocation nativa AOT.
 * @param k Kind de la relocation.
 * @return Cadena ASCII.
 */
const char *reloc_kind_str(jit::NativeReloc::Kind k) {
    switch (k) {
    case jit::NativeReloc::Kind::CALL_REL32: return "CALL_REL32";
    case jit::NativeReloc::Kind::ABS64: return "ABS64";
    case jit::NativeReloc::Kind::DATA_REL32: return "DATA_REL32";
    }
    return "?";
}

} // namespace

/**
 * @struct Inspector::JitState
 * @brief Subsistema JIT propio del inspector (aislado del runtime).
 *
 * Mantiene un @c CodeCache + @c RuntimeEntries + @c JitCompiler dedicados
 * para compilar funciones a x86-64 sin interferir con el JIT del proceso.
 * El mismo patron que usa @c auto_jit al inicializar su singleton.
 */
struct Inspector::JitState {
    jit::CodeCache cache;        ///< Cache de codigo nativo del inspector.
    jit::RuntimeEntries entries; ///< Punteros a runtime entries resueltos.
    jit::JitCompiler compiler;   ///< Compilador JIT.

    JitState() : cache(), entries(), compiler(cache, entries) {
        // Resolver los simbolos vrt_* una vez (estables durante el proceso).
        entries.resolve();
    }
};

Inspector::Inspector(AnalysisEngine &engine, DocumentStore &docs) noexcept
    : engine_(engine), docs_(docs) {}

Inspector::~Inspector() = default;

Inspector::JitState *Inspector::jit_state() {
    if (jit_)
        return jit_.get();
    if (jit_init_failed_)
        return nullptr;
    try {
        jit_ = std::make_unique<JitState>();
    } catch (...) {
        // Si la inicializacion del JIT falla, no reintentar en cada peticion.
        jit_init_failed_ = true;
        return nullptr;
    }
    return jit_.get();
}

nlohmann::json Inspector::bytecode(const std::string &uri) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const DocAnalysis &an = engine_.analyze_document(uri, docs_.text(uri));
    nlohmann::json out;
    out["text"] = an.result.vel_text;
    return out;
}

nlohmann::json Inspector::ir(const std::string &uri, const std::string &phase) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const std::string &text = docs_.text(uri);

    if (phase == "pre") {
        // El IR pre-opt NO esta en el CompileResult cacheado por defecto:
        // exige recompilar con emit_ir_preopt.  Cachear por (uri, hash).
        const uint64_t h = fnv1a_hash(text);
        const std::string key = uri + "|" + std::to_string(h) + "|ir-pre";
        auto it = view_cache_.find(key);
        if (it != view_cache_.end())
            return {{"text", it->second}};

        vex::CompileOptions opts;
        opts.module_name = "main";
        opts.emit_ir_preopt = true;
        vex::CompileResult res = vex::compile_vex_source(text, uri, opts);
        if (res.ir_module_cache_bytes_preopt.empty())
            return {{"error", "no se pudo generar el IR pre-optimizacion"}};
        ir::IrModule mod;
        if (!ir::parse_ir_module_cache(res.ir_module_cache_bytes_preopt, mod))
            return {{"error", "no se pudo deserializar el IR pre-optimizacion"}};
        std::ostringstream oss;
        ir::ir_print(mod, oss);
        std::string rendered = oss.str();
        view_cache_[key] = rendered;
        return {{"text", std::move(rendered)}};
    }

    // phase "post" (o cualquier otra): reutilizar el cache del motor.
    const DocAnalysis &an = engine_.analyze_document(uri, text);
    ir::IrModule mod;
    if (!parse_post_opt_module(an.result, mod))
        return {{"error", "el modulo no produjo IR (revisa los diagnosticos)"}};
    std::ostringstream oss;
    ir::ir_print(mod, oss);
    return {{"text", oss.str()}};
}

nlohmann::json Inspector::complexity(const std::string &uri) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const DocAnalysis &an = engine_.analyze_document(uri, docs_.text(uri));
    ir::IrModule mod;
    if (!parse_post_opt_module(an.result, mod))
        return {{"error", "el modulo no produjo IR (revisa los diagnosticos)"}};

    // Coste PARCIAL + composicion interprocedural (coste efectivo real).
    analyze::ModuleCost mc = analyze::analyze_module(mod);
    analyze::compose_interproc(mc);

    nlohmann::json arr = nlohmann::json::array();
    for (const auto &cr : mc.functions) {
        nlohmann::json jf;
        jf["name"] = cr.function;
        jf["partial"] = analyze::cost_class_str(cr.big_o);
        jf["total"] = analyze::cost_class_str(cr.total_class);
        // Confianza: 0=EXACT, 1=HEURISTIC, 2=UNKNOWN.
        jf["confidence"] = static_cast<int>(cr.confidence);
        jf["total_confidence"] = static_cast<int>(cr.total_confidence);
        jf["max_loop_depth"] = cr.max_loop_depth;
        jf["recursive"] = cr.is_recursive;
        jf["divide_conquer"] = cr.is_divide_conquer;
        jf["declared"] = cr.declared_expr; // vacio => sin contrato @complexity.
        jf["contract_mismatch"] = cr.contract_mismatch;
        arr.push_back(std::move(jf));
    }
    nlohmann::json out;
    out["functions"] = std::move(arr);
    return out;
}

nlohmann::json Inspector::diagram(const std::string &uri,
                                  const std::string &kind,
                                  const std::string &format, bool cost) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const std::string &text = docs_.text(uri);

    // Validar kind y format antes de recompilar.
    const bool kind_ok = (kind == "ast" || kind == "ir-pre" ||
                          kind == "ir-post" || kind == "vel");
    if (!kind_ok)
        return {{"error", "kind invalido (use ast|ir-pre|ir-post|vel)"}};
    const bool fmt_ok =
        (format == "mermaid" || format == "graphviz" || format == "html");
    if (!fmt_ok)
        return {{"error", "format invalido (use mermaid|graphviz|html)"}};

    // Cache por (uri, hash, kind, format, cost): la generacion de diagramas
    // recompila con flags concretos -> no repetir peticiones identicas.
    const uint64_t h = fnv1a_hash(text);
    const std::string key = uri + "|" + std::to_string(h) + "|diag:" + kind +
                            ":" + format + ":" + (cost ? "1" : "0");
    auto it = view_cache_.find(key);
    if (it != view_cache_.end())
        return {{"text", it->second}};

    // Activar SOLO el flag de la vista pedida (uno por kind x format).
    vex::CompileOptions opts;
    opts.module_name = "main";
    opts.annotate_cost = cost;
    if (format == "mermaid") {
        if (kind == "ast") opts.dump_mermaid_ast = true;
        else if (kind == "ir-pre") opts.dump_mermaid_ir_pre = true;
        else if (kind == "ir-post") opts.dump_mermaid_ir_post = true;
        else opts.dump_mermaid_vel = true;
    } else if (format == "graphviz") {
        if (kind == "ast") opts.dump_graphviz_ast = true;
        else if (kind == "ir-pre") opts.dump_graphviz_ir_pre = true;
        else if (kind == "ir-post") opts.dump_graphviz_ir_post = true;
        else opts.dump_graphviz_vel = true;
    } else { // html
        if (kind == "ast") opts.dump_html_ast = true;
        else if (kind == "ir-pre") opts.dump_html_ir_pre = true;
        else if (kind == "ir-post") opts.dump_html_ir_post = true;
        else opts.dump_html_vel = true;
    }

    vex::CompileResult res = vex::compile_vex_source(text, uri, opts);
    // Seleccionar el campo del CompileResult que corresponde a la vista.
    std::string out_text;
    if (format == "mermaid") {
        if (kind == "ast") out_text = res.mermaid_ast;
        else if (kind == "ir-pre") out_text = res.mermaid_ir_pre;
        else if (kind == "ir-post") out_text = res.mermaid_ir_post;
        else out_text = res.mermaid_vel;
    } else if (format == "graphviz") {
        if (kind == "ast") out_text = res.graphviz_ast;
        else if (kind == "ir-pre") out_text = res.graphviz_ir_pre;
        else if (kind == "ir-post") out_text = res.graphviz_ir_post;
        else out_text = res.graphviz_vel;
    } else {
        if (kind == "ast") out_text = res.html_ast;
        else if (kind == "ir-pre") out_text = res.html_ir_pre;
        else if (kind == "ir-post") out_text = res.html_ir_post;
        else out_text = res.html_vel;
    }

    if (out_text.empty())
        return {{"error",
                 "el diagrama salio vacio (revisa los diagnosticos del fuente)"}};
    view_cache_[key] = out_text;
    return {{"text", std::move(out_text)}};
}

nlohmann::json Inspector::functions(const std::string &uri) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const DocAnalysis &an = engine_.analyze_document(uri, docs_.text(uri));
    ir::IrModule mod;
    if (!parse_post_opt_module(an.result, mod))
        return {{"error", "el modulo no produjo IR (revisa los diagnosticos)"}};

    nlohmann::json arr = nlohmann::json::array();
    for (const auto &fn : mod.functions) {
        // Saltar stubs nativos y funciones macro-compiladas (no son del
        // codigo del usuario en el sentido habitual).
        if (fn.is_native || fn.is_macro_compiled)
            continue;
        nlohmann::json jf;
        jf["name"] = fn.name;
        jf["line"] = first_source_line(fn);
        arr.push_back(std::move(jf));
    }
    nlohmann::json out;
    out["functions"] = std::move(arr);
    return out;
}

nlohmann::json Inspector::aot_compat(const std::string &uri,
                                     const std::string &tier) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const DocAnalysis &an = engine_.analyze_document(uri, docs_.text(uri));
    ir::IrModule mod;
    if (!parse_post_opt_module(an.result, mod))
        return {{"error", "el modulo no produjo IR (revisa los diagnosticos)"}};

    aot::AotTarget target;
    target.tier = tier_from_str(tier);
    aot::AotCompatReport report = aot::aot_analyze_module(mod, target);

    nlohmann::json issues = nlohmann::json::array();
    for (const auto &iss : report.issues) {
        nlohmann::json ji;
        ji["fn_name"] = iss.fn_name;
        ji["source_line"] = iss.source_line;
        ji["op"] = ir::ir_op_name(iss.op);
        ji["reason"] = iss.reason;
        issues.push_back(std::move(ji));
    }
    nlohmann::json ok = nlohmann::json::array();
    for (const auto &name : report.ok_functions)
        ok.push_back(name);

    nlohmann::json out;
    out["tier"] = tier.empty() ? std::string("bare") : tier;
    out["compatible"] = report.compatible;
    out["issues"] = std::move(issues);
    out["ok_functions"] = std::move(ok);
    return out;
}

nlohmann::json Inspector::jit_asm(const std::string &uri,
                                  const std::string &function) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const DocAnalysis &an = engine_.analyze_document(uri, docs_.text(uri));
    ir::IrModule mod;
    if (!parse_post_opt_module(an.result, mod))
        return {{"error", "el modulo no produjo IR (revisa los diagnosticos)"}};

    const ir::IrFunction *fn = pick_function(mod, function);
    if (!fn) {
        if (!function.empty())
            return {{"error", "funcion no encontrada: " + function}};
        return {{"error", "el modulo no tiene funciones compilables"}};
    }

    JitState *st = jit_state();
    if (!st)
        return {{"error", "no se pudo inicializar el subsistema JIT"}};

    // Compilar la funcion aislada en convencion NATIVE_ABI (vista autonoma,
    // sin depender de un ProcessVM en ejecucion).
    jit::CompileResult cr =
        st->compiler.compile(*fn, jit::SelectorMode::NATIVE_ABI);
    if (!cr.fn || !cr.code_start) {
        if (cr.unsupported)
            return {{"unsupported", true},
                    {"reason",
                     "la funcion '" + fn->name +
                         "' usa operaciones IR no soportadas por el selector "
                         "JIT (v1: aritmetica/loops/llamadas basicas)"}};
        return {{"error", "el JIT no produjo codigo para '" + fn->name + "'"}};
    }

    // Desensamblar los bytes generados.  La base 0 da offsets relativos.
    std::string text = disasm_x86_64(cr.code_start, cr.code_size, 0);
    // Liberar el codigo: una vista no necesita conservarlo ejecutable.
    st->compiler.invalidate(cr);

    nlohmann::json out;
    out["text"] = std::move(text);
    out["function"] = fn->name;
    out["bytes"] = static_cast<uint64_t>(cr.code_size);
    out["instructions"] = static_cast<uint64_t>(cr.instr_count);
    return out;
}

nlohmann::json Inspector::aot_asm(const std::string &uri,
                                  const std::string &function) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const DocAnalysis &an = engine_.analyze_document(uri, docs_.text(uri));
    ir::IrModule mod;
    if (!parse_post_opt_module(an.result, mod))
        return {{"error", "el modulo no produjo IR (revisa los diagnosticos)"}};

    const ir::IrFunction *fn = pick_function(mod, function);
    if (!fn) {
        if (!function.empty())
            return {{"error", "funcion no encontrada: " + function}};
        return {{"error", "el modulo no tiene funciones compilables"}};
    }

    // Primero comprobar compatibilidad AOT (tier BARE: el subset mas
    // estricto, lo que el codegen nativo aislado puede materializar).
    aot::AotTarget target;
    target.tier = aot::Tier::BARE;
    std::vector<aot::AotIncompat> issues;
    if (!aot::aot_analyze_function(*fn, target, issues)) {
        std::string reason = "la funcion '" + fn->name +
                             "' no es compatible con AOT bare";
        if (!issues.empty()) {
            reason += ": ";
            reason += ir::ir_op_name(issues.front().op);
            reason += " (";
            reason += issues.front().reason;
            reason += ")";
        }
        return {{"incompatible", true}, {"reason", reason}};
    }

    // Compilar a bytes nativos (HOST_LEAF) con resolvers vacios: vista
    // aislada de UNA funcion (las CALL cross-funcion / datos quedan como
    // relocations sin resolver, que se reportan al cliente).
    std::vector<jit::NativeReloc> relocs;
    std::vector<uint8_t> bytes;
    try {
        bytes = jit::vreg_compile_native(*fn, /*resolve_call=*/{},
                                         /*ent=*/{}, /*resolve_native=*/{},
                                         /*resolve_symbol=*/{}, &relocs);
    } catch (...) {
        return {{"error", "el codegen AOT lanzo una excepcion para '" +
                              fn->name + "'"}};
    }
    if (bytes.empty())
        return {{"incompatible", true},
                {"reason", "la funcion '" + fn->name +
                               "' no esta soportada por el selector vreg AOT"}};

    std::string text = disasm_x86_64(bytes.data(), bytes.size(), 0);

    nlohmann::json jrelocs = nlohmann::json::array();
    for (const auto &r : relocs) {
        nlohmann::json jr;
        jr["offset"] = r.offset;
        jr["kind"] = reloc_kind_str(r.kind);
        jr["symbol"] = r.symbol;
        jr["addend"] = r.addend;
        jrelocs.push_back(std::move(jr));
    }

    nlohmann::json out;
    out["text"] = std::move(text);
    out["function"] = fn->name;
    out["bytes"] = static_cast<uint64_t>(bytes.size());
    out["relocs"] = std::move(jrelocs);
    return out;
}

} // namespace lsp
