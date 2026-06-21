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
 * @file analysis_engine.cpp
 * @brief Implementacion del motor de analisis de documentos del LSP.
 *
 * RIESGO conocido (fase futura): el frontend del compilador podria llegar
 * a segfaultear con fuente Vex muy parcial o malformada.  Las excepciones
 * C++ SI se capturan aqui, pero un segfault no.  Una fase futura debe
 * aislar el analisis en un subproceso o thread con sandbox para que un
 * crash del frontend no tumbe el servidor LSP.  De momento se confia en
 * que el frontend reporta errores via diagnosticos y no aborta.
 */

#include "lsp/analysis_engine.h"

#include <exception>

#include "analyze/bigo.h"
#include "ir/ssa_ir.h"
#include "ir/ssa_ir_serialize.h"
#include "vex/ast.h"
#include "vex/diagnostic.h"
#include "vex/lexer.h"
#include "vex/parser.h"

namespace lsp {

uint64_t fnv1a_hash(const std::string &s) {
    // FNV-1a 64-bit: rapido, sin dependencias, suficiente para detectar
    // cambios del texto entre peticiones (no es hash criptografico).
    uint64_t h = 1469598103934665603ULL; // offset basis
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL; // prime
    }
    return h;
}

namespace {

/**
 * @brief Devuelve la primera linea fuente conocida de una funcion IR.
 *
 * Recorre los bloques y sus instrucciones buscando el primer
 * @c source_line distinto de cero.  Sirve para ubicar de forma
 * best-effort el warning de @c @complexity cuando la cota inferida no
 * coincide con la declarada (el IR no guarda una SourceLoc por funcion).
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
 * @brief Engancha el warning de discrepancia de @c @complexity.
 *
 * Deserializa el IR del modulo (post-opt) cacheado en el @c CompileResult,
 * corre el analisis de coste interprocedural y, por cada funcion cuyo
 * contrato @c @complexity no coincide de forma confirmada con la cota
 * inferida (@c contract_mismatch), emite un WARNING.
 *
 * Best-effort en la ubicacion: el IR no conserva una SourceLoc por
 * funcion, asi que el warning se situa en la primera linea conocida de la
 * funcion (columna 1).  Si no hay linea, se omite el warning de esa
 * funcion para no apuntar a una posicion enganosa.
 *
 * Robusto: cualquier fallo de deserializacion o analisis se traga en
 * silencio (el warning es informativo, no debe romper los diagnosticos
 * principales).
 *
 * @param result CompileResult con @c ir_module_cache_bytes; recibe los
 *               warnings en @c result.diagnostics.
 * @param filename Nombre logico del fichero para la SourceLoc.
 */
void attach_complexity_warnings(vex::CompileResult &result,
                                const std::string &filename) {
    if (result.ir_module_cache_bytes.empty())
        return;
    try {
        ir::IrModule mod;
        if (!ir::parse_ir_module_cache(result.ir_module_cache_bytes, mod))
            return;
        // Coste PARCIAL + composicion interprocedural (coste efectivo real).
        analyze::ModuleCost mc = analyze::analyze_module(mod);
        analyze::compose_interproc(mc);

        // Indexar las funciones IR por nombre para recuperar su linea.
        // mc.functions sigue el orden de mod.functions, asi que un recorrido
        // emparejado es O(n) sin construir mapa.
        for (const auto &cr : mc.functions) {
            if (!cr.contract_mismatch)
                continue;
            // Localizar la IrFunction homonima para extraer la linea.
            uint32_t line = 0;
            for (const auto &fn : mod.functions) {
                if (fn.name == cr.function) {
                    line = first_source_line(fn);
                    break;
                }
            }
            if (line == 0)
                continue; // sin linea fiable: no ubicar el warning.

            vex::SourceLoc loc;
            loc.file = filename;
            loc.line = line;
            loc.column = 1;
            loc.length = 1;
            // Mensaje claro: declarada vs inferida (la efectiva post-opt).
            std::string msg = "la complejidad declarada (@complexity ";
            msg += cr.declared_expr;
            msg += ") no coincide con la inferida (";
            msg += analyze::cost_class_str(cr.total_class);
            msg += ")";
            result.diagnostics.warning(loc, msg);
        }
    } catch (...) {
        // El warning de complejidad es opcional: ignorar cualquier fallo.
    }
}

/**
 * @brief Extrae los nombres de tipos/clases/structs/enums/funciones
 *        declarados top-level en el fuente.
 *
 * Hace un lex+parse independiente (descartando los diagnosticos: ya los
 * produce la compilacion principal) y recorre las declaraciones top-level
 * del @c ModuleNode para poblar los sets de @p out.  Se ejecuta bajo
 * try/catch externo en el caller; aqui ademas se ignora cualquier nodo
 * inesperado para ser robusto frente a AST parciales (fuente a medio
 * teclear).
 *
 * Este parse extra es necesario porque @c CompileResult no expone las
 * tablas de tipos del type checker; el coste es pequeno (el lexer es
 * O(N) y solo recorremos las declaraciones de primer nivel).
 *
 * @param text Texto fuente del documento.
 * @param uri  URI logico (nombre del fichero para el lexer).
 * @param out  DocAnalysis cuyos sets de nombres se rellenan.
 */
void extract_declared_names(const std::string &text, const std::string &uri,
                            DocAnalysis &out) {
    // Diagnosticos locales y descartables: solo queremos el AST.
    vex::Diagnostics local_diags;
    vex::Lexer lex(text, uri, local_diags);
    vex::Parser parser(lex, local_diags);
    std::unique_ptr<vex::ast::ModuleNode> mod = parser.parse_program();
    if (!mod)
        return;
    // Recorrer SOLO las declaraciones top-level: nombres de tipos y funciones
    // visibles para el resaltado.  Los miembros (campos/metodos) se refinaran
    // en una fase futura (parametros/propiedades por posicion).
    for (const auto &node : mod->decls) {
        if (!node)
            continue;
        switch (node->kind) {
        case vex::ast::NodeKind::ClassDecl: {
            auto *d = static_cast<const vex::ast::ClassDecl *>(node.get());
            if (!d->name.empty())
                out.class_names.insert(d->name);
            break;
        }
        case vex::ast::NodeKind::StructDecl: {
            auto *d = static_cast<const vex::ast::StructDecl *>(node.get());
            if (!d->name.empty())
                out.struct_names.insert(d->name);
            break;
        }
        case vex::ast::NodeKind::EnumDecl: {
            auto *d = static_cast<const vex::ast::EnumDecl *>(node.get());
            if (!d->name.empty())
                out.enum_names.insert(d->name);
            break;
        }
        case vex::ast::NodeKind::TypeAliasDecl: {
            auto *d = static_cast<const vex::ast::TypeAliasDecl *>(node.get());
            if (!d->name.empty())
                out.type_names.insert(d->name);
            break;
        }
        case vex::ast::NodeKind::FunctionDecl: {
            auto *d = static_cast<const vex::ast::FunctionDecl *>(node.get());
            if (!d->name.empty())
                out.function_names.insert(d->name);
            break;
        }
        case vex::ast::NodeKind::ExternFnDecl: {
            // Las funciones extern (FFI declarativo) tambien se clasifican
            // como funciones para el resaltado.
            auto *d = static_cast<const vex::ast::ExternFnDecl *>(node.get());
            if (!d->name.empty())
                out.function_names.insert(d->name);
            break;
        }
        default:
            break;
        }
    }
}

} // namespace

const DocAnalysis &AnalysisEngine::analyze_document(const std::string &uri,
                                                    const std::string &text) {
    const uint64_t h = fnv1a_hash(text);

    // Cache hit: mismo texto, mismo resultado.  Evita recompilar.
    auto it = cache_.find(uri);
    if (it != cache_.end() && it->second->text_hash == h)
        return *it->second;

    // Crear (o reusar el slot de) el analisis para este documento.
    auto analysis = std::make_unique<DocAnalysis>();
    analysis->text_hash = h;

    try {
        // Compilar el fuente.  No aplicamos VPP en Fase 1 (el frontend no lo
        // hace por si mismo; integrarlo es trabajo de una fase posterior).
        vex::CompileOptions opts;
        opts.module_name = "main";
        analysis->result = vex::compile_vex_source(text, uri, opts);
        // Enganchar (best-effort) el warning de discrepancia de @complexity.
        attach_complexity_warnings(analysis->result, uri);
        // Poblar los sets de nombres declarados para el resaltado semantico.
        // Best-effort: un fallo del parse extra no debe afectar a los
        // diagnosticos (el catch externo cubre cualquier excepcion).
        extract_declared_names(text, uri, *analysis);
    } catch (const std::exception &e) {
        // Un fallo del frontend NO debe tumbar el servidor: convertirlo en un
        // diagnostico de error interno en 0:0 para que el cliente lo vea.
        analysis->result = vex::CompileResult{};
        analysis->result.ok = false;
        vex::SourceLoc loc;
        loc.file = uri;
        loc.line = 1; // LSP es 0-based; el mapeo restara 1 -> linea 0.
        loc.column = 1;
        loc.length = 1;
        std::string msg = "error interno del analizador: ";
        msg += e.what();
        analysis->result.diagnostics.error(loc, msg);
    } catch (...) {
        // Captura de cualquier otra excepcion no estandar.
        analysis->result = vex::CompileResult{};
        analysis->result.ok = false;
        vex::SourceLoc loc;
        loc.file = uri;
        loc.line = 1;
        loc.column = 1;
        loc.length = 1;
        analysis->result.diagnostics.error(
            loc, "error interno del analizador (excepcion desconocida)");
    }

    // Insertar/actualizar la cache y devolver la referencia estable.
    DocAnalysis &ref = *analysis;
    cache_[uri] = std::move(analysis);
    return ref;
}

const DocAnalysis *AnalysisEngine::cached(const std::string &uri) const {
    auto it = cache_.find(uri);
    return it == cache_.end() ? nullptr : it->second.get();
}

void AnalysisEngine::forget(const std::string &uri) { cache_.erase(uri); }

} // namespace lsp
