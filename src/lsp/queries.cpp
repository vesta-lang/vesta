/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file lsp/queries.cpp
 * @brief Las filas: una por consulta, con sus parametros, sus defectos y lo
 *        que hace.
 *
 * Esta es la lista que ANTES estaba escrita dos veces -- la que se anuncia al
 * cliente y la cadena de comparaciones que despacha --, y que ya habia
 * divergido en tres consultas.  Ahora se escribe aqui y las dos salen de ella.
 *
 * Al anadir una consulta: se anade la fila y ya esta.  Se publica sola, se
 * despacha sola, y su esquema lo genera quien lo necesite.
 */

#include "lsp/queries.h"

#include "lsp/document_store.h"
#include "lsp/param_hints.h"

namespace lsp {

using query::Args;
using query::Ctx;
using query::DefaultValue;
using query::Effect;
using query::Needs;
using query::optional;
using query::QueryDesc;
using query::QueryParam;
using query::required;
namespace p = query::p;

namespace {

/// El contexto del inspector, ya comprobado.  Un manejador que llegue aqui con
/// otro contexto es un fallo de cableado, no una peticion mala: se ve al primer
/// intento y no tiene por que devolver un resultado plausible.
InspectorCtx &self(Ctx &c) {
    return static_cast<InspectorCtx &>(c);
}

// -------------------------------------------------------------------------
// Parametros propios de cada consulta.  Los comunes (`uri` y los cinco del
// objetivo) NO se repiten: los aporta el @c Needs de la fila.
// -------------------------------------------------------------------------

/* Cada parametro se COMPONE del catalogo (@c query::p), nunca se vuelve a
 * declarar: su nombre, su tipo y su explicacion viven en un solo sitio, y aqui
 * solo se dice si hace falta y con que se queda si no viene -- que es lo unico
 * que de verdad cambia de una consulta a otra. */

constexpr QueryParam kFunctionOpt[] = {
    optional(p::function, DefaultValue::str("")),
};

constexpr QueryParam kIrParams[] = {
    optional(p::phase, DefaultValue::str("post")),
};

constexpr QueryParam kDiagramParams[] = {
    optional(p::kind, DefaultValue::str("ir-post")),
    optional(p::format, DefaultValue::str("mermaid")),
    optional(p::cost, DefaultValue::boolean(false)),
    // Solo lo usa kind="asm" (grafo de flujo del codigo nativo).
    optional(p::function, DefaultValue::str("")),
};

constexpr QueryParam kTierParams[] = {
    optional(p::tier, DefaultValue::str("bare")),
};

constexpr QueryParam kModesParams[] = {
    optional(p::mode, DefaultValue::str("")),
    optional(p::tier, DefaultValue::str("bare")),
};

/* La linea es OBLIGATORIA aqui y opcional en el simbolo bajo el cursor: es el
 * mismo parametro con distinto papel, que es justo lo que el catalogo deja
 * expresar sin duplicar su descripcion. */
constexpr QueryParam kLineCpuArch[] = {
    required(p::line),
    optional(p::cpu, DefaultValue::str("")),
    optional(p::arch, DefaultValue::str("")),
};

constexpr QueryParam kArchOnly[] = {
    optional(p::arch, DefaultValue::str("")),
};

// -------------------------------------------------------------------------
// Los manejadores.  Cada uno lee SOLO lo que su fila declara: pedir otra cosa
// es un fallo ruidoso, no un cero callado.
// -------------------------------------------------------------------------

nlohmann::json q_targets(Ctx &c, const Args &) {
    return self(c).inspector->targets();
}

nlohmann::json q_bytecode(Ctx &c, const Args &a) {
    InspectorCtx &s = self(c);
    return s.inspector->bytecode(a.str("uri"), a.str("function"), s.target);
}

nlohmann::json q_ir(Ctx &c, const Args &a) {
    InspectorCtx &s = self(c);
    return s.inspector->ir(a.str("uri"), a.str("phase"), s.target);
}

nlohmann::json q_ir_diff(Ctx &c, const Args &a) {
    return self(c).inspector->ir_diff(a.str("uri"), a.str("function"));
}

nlohmann::json q_complexity(Ctx &c, const Args &a) {
    return self(c).inspector->complexity(a.str("uri"));
}

nlohmann::json q_function_report(Ctx &c, const Args &a) {
    return self(c).inspector->function_report(a.str("uri"));
}

nlohmann::json q_diagram(Ctx &c, const Args &a) {
    InspectorCtx &s = self(c);
    return s.inspector->diagram(a.str("uri"), a.str("kind"), a.str("format"),
                                a.boolean("cost"), s.target,
                                a.str("function"));
}

nlohmann::json q_functions(Ctx &c, const Args &a) {
    return self(c).inspector->functions(a.str("uri"));
}

nlohmann::json q_aot_compat(Ctx &c, const Args &a) {
    return self(c).inspector->aot_compat(a.str("uri"), a.str("tier"));
}

nlohmann::json q_jit_asm(Ctx &c, const Args &a) {
    InspectorCtx &s = self(c);
    return s.inspector->jit_asm(a.str("uri"), a.str("function"), s.target);
}

nlohmann::json q_aot_asm(Ctx &c, const Args &a) {
    InspectorCtx &s = self(c);
    return s.inspector->aot_asm(a.str("uri"), a.str("function"), s.target);
}

nlohmann::json q_modes(Ctx &c, const Args &a) {
    return self(c).inspector->modes(a.str("uri"), a.str("mode"), a.str("tier"));
}

nlohmann::json q_macro_expand(Ctx &c, const Args &a) {
    return self(c).inspector->macro_expand(a.str("uri"));
}

nlohmann::json q_comptime_values(Ctx &c, const Args &a) {
    return self(c).inspector->comptime_values(a.str("uri"));
}

nlohmann::json q_asa(Ctx &c, const Args &a) {
    return self(c).inspector->asa(a.str("uri"));
}

nlohmann::json q_asa_facts(Ctx &c, const Args &a) {
    return self(c).inspector->asa_facts(a.str("uri"));
}

nlohmann::json q_instruction(Ctx &c, const Args &a) {
    return self(c).inspector->instruction(a.str("uri"), a.uint("line"),
                                          a.str("cpu"), a.str("arch"));
}

nlohmann::json q_asm_block(Ctx &c, const Args &a) {
    return self(c).inspector->asm_block(a.str("uri"), a.uint("line"),
                                        a.str("cpu"), a.str("arch"));
}

nlohmann::json q_asm_flow(Ctx &c, const Args &a) {
    return self(c).inspector->asm_flow(a.str("uri"), a.str("arch"));
}

nlohmann::json q_param_hints(Ctx &c, const Args &a) {
    InspectorCtx &s = self(c);
    const std::string uri = a.str("uri");
    nlohmann::json arr = nlohmann::json::array();
    if (s.docs != nullptr && s.docs->has(uri)) {
        const auto text_ref = s.docs->text(uri);
        for (const ParamHint &h : compute_param_hints(*text_ref, uri)) {
            nlohmann::json o;
            o["line"] = h.line;
            o["character"] = h.character;
            o["label"] = h.label;
            arr.push_back(std::move(o));
        }
    }
    nlohmann::json out = nlohmann::json::object();
    out["hints"] = std::move(arr);
    return out;
}

nlohmann::json q_symbol_info(Ctx &c, const Args &a) {
    return self(c).host->symbol_info(a.str("uri"), a.uint("line"),
                                     a.uint("character"));
}

nlohmann::json q_compile(Ctx &c, const Args &a) {
    /* El driver de compilacion lee los parametros con SU vocabulario (salida,
     * formato, objetivo), que no es el de la tabla; se le pasan crudos a
     * proposito en vez de duplicar aqui una lista que ya vive alli. */
    return self(c).host->compile(a.str("uri"), false, *a.raw);
}

nlohmann::json q_compile_project(Ctx &c, const Args &a) {
    return self(c).host->compile(a.str("uri"), true, *a.raw);
}

constexpr QueryParam kPositionParams[] = {
    optional(p::line, DefaultValue::uint(0)),
    optional(p::character, DefaultValue::uint(0)),
};

// -------------------------------------------------------------------------
// LA TABLA.
// -------------------------------------------------------------------------
//
// El nombre va SIN prefijo: el editor lo publica como `vesta/<nombre>`, y otro
// transporte lo decorara a su manera.  Escribirlo decorado aqui obligaria a
// cada cara a des-decorarlo, que es otra copia del nombre.

constexpr QueryDesc kQueries[] = {
    // --- el catalogo de objetivos no habla de un documento ---
    {"targets", "QRY.targets", {}, Needs::None, Effect::ReadOnly, q_targets},

    // --- el programa ---
    {"functions", "QRY.functions", {}, Needs::Document, Effect::ReadOnly,
     q_functions},
    {"bytecode", "QRY.bytecode", kFunctionOpt,
     Needs::Document | Needs::Target, Effect::ReadOnly, q_bytecode},
    {"ir", "QRY.ir", kIrParams, Needs::Document | Needs::Target,
     Effect::ReadOnly, q_ir},
    {"irDiff", "QRY.irDiff", kFunctionOpt, Needs::Document, Effect::ReadOnly,
     q_ir_diff},
    {"diagram", "QRY.diagram", kDiagramParams,
     Needs::Document | Needs::Target, Effect::ReadOnly, q_diagram},
    {"macroExpand", "QRY.macroExpand", {}, Needs::Document, Effect::ReadOnly,
     q_macro_expand},
    {"comptimeValues", "QRY.comptimeValues", {}, Needs::Document,
     Effect::ReadOnly, q_comptime_values},

    // --- lo que el compilador SABE ---
    {"asa", "QRY.asa", {}, Needs::Document, Effect::ReadOnly, q_asa},
    {"asaFacts", "QRY.asaFacts", {}, Needs::Document, Effect::ReadOnly,
     q_asa_facts},
    {"complexity", "QRY.complexity", {}, Needs::Document, Effect::ReadOnly,
     q_complexity},
    {"functionReport", "QRY.functionReport", {}, Needs::Document,
     Effect::ReadOnly, q_function_report},

    // --- el codigo maquina ---
    {"jitAsm", "QRY.jitAsm", kFunctionOpt, Needs::Document | Needs::Target,
     Effect::ReadOnly, q_jit_asm},
    {"aotAsm", "QRY.aotAsm", kFunctionOpt, Needs::Document | Needs::Target,
     Effect::ReadOnly, q_aot_asm},
    {"aotCompat", "QRY.aotCompat", kTierParams, Needs::Document,
     Effect::ReadOnly, q_aot_compat},
    // Estas tres piden la linea porque se contesta por lo que el compilador
    // ENTENDIo de ella, no por su texto.  Llevan `cpu`/`arch` propios y no el
    // paquete del objetivo: no compilan una vista, consultan la base de
    // instrucciones.
    {"instruction", "QRY.instruction", kLineCpuArch, Needs::Document,
     Effect::ReadOnly, q_instruction},
    {"asmBlock", "QRY.asmBlock", kLineCpuArch, Needs::Document,
     Effect::ReadOnly, q_asm_block},
    {"asmFlow", "QRY.asmFlow", kArchOnly, Needs::Document, Effect::ReadOnly,
     q_asm_flow},

    // --- ayudas del editor ---
    {"paramHints", "QRY.paramHints", {}, Needs::Document, Effect::ReadOnly,
     q_param_hints},
    {"symbolInfo", "QRY.symbolInfo", kPositionParams, Needs::Document,
     Effect::ReadOnly, q_symbol_info},

    // --- ESCRIBEN: dejan un artefacto en disco ---
    {"compile", "QRY.compile", {}, Needs::Document, Effect::Writes, q_compile},
    {"compileProject", "QRY.compileProject", {},
     Needs::Document | Needs::Project, Effect::Writes, q_compile_project},

    // --- EJECUTA: corre el modulo en interprete / JIT / AOT y compara ---
    {"modes", "QRY.modes", kModesParams, Needs::Document, Effect::Executes,
     q_modes},
};

} // namespace

void register_inspector_queries() {
    query::Registry::instance().add_array(kQueries);
}

} // namespace lsp
