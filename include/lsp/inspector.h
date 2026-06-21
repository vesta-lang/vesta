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
 * @file inspector.h
 * @brief Inspector del ecosistema Vex para el LSP (Fase 3).
 *
 * Implementa las peticiones a medida @c vesta/* que el editor llama
 * BAJO DEMANDA (el usuario pulsa "ver IR / bytecode / JIT / AOT / ...")
 * para visualizar cada fase del pipeline sobre el documento abierto:
 *
 *   - @c vesta/bytecode    -> texto @c .vel del modulo.
 *   - @c vesta/ir          -> SSA IR pre/post optimizacion.
 *   - @c vesta/complexity  -> coste Big-O por funcion (parcial + total).
 *   - @c vesta/diagram     -> diagrama AST/IR/VEL en mermaid/graphviz/html.
 *   - @c vesta/functions   -> lista de funciones (nombre + linea).
 *   - @c vesta/aotCompat   -> reporte de compatibilidad AOT (bare/embed/full).
 *   - @c vesta/jitAsm      -> desensamblado x86-64 del codigo JIT de una fn.
 *   - @c vesta/aotAsm      -> desensamblado x86-64 del codigo AOT de una fn.
 *
 * Diseno:
 *  - Reutiliza el @c CompileResult cacheado por @c AnalysisEngine para las
 *    vistas baratas (bytecode, ir-post, complexity, functions, aotCompat).
 *  - Para las vistas caras que exigen recompilar con flags concretos
 *    (ir-pre con @c emit_ir_preopt, diagramas con @c dump_*), mantiene una
 *    cache PROPIA por (uri, hash del texto, clave de la vista) para no
 *    recompilar en cada peticion identica.
 *  - El codigo nativo del JIT y del AOT se desensambla con Capstone
 *    (x86-64).  El subsistema JIT del inspector es PROPIO (CodeCache +
 *    RuntimeEntries + JitCompiler dedicados) para no interferir con el
 *    runtime; se instancia perezosamente en la primera peticion @c jitAsm.
 *  - TODA peticion se sirve bajo try/catch en el caller: ante un fallo el
 *    inspector devuelve un resultado con @c error / @c unsupported en lugar
 *    de propagar la excepcion (el servidor NUNCA muere).
 */

#ifndef VESTA_LSP_INSPECTOR_H
#define VESTA_LSP_INSPECTOR_H

#include <memory>
#include <string>
#include <unordered_map>

#include "json.hpp"

#include "lsp/analysis_engine.h"

namespace lsp {

class DocumentStore;

/**
 * @class Inspector
 * @brief Sirve las peticiones @c vesta/* del LSP (vistas del ecosistema).
 *
 * No es thread-safe: el servidor procesa peticiones secuencialmente.  Cada
 * metodo devuelve un @c nlohmann::json con la forma de respuesta del
 * protocolo (campos documentados en cada metodo); ante un error controlado
 * devuelve un objeto con @c {"error": "<motivo>"} para que el cliente lo
 * muestre sin tumbar la sesion.
 */
class Inspector {
  public:
    /**
     * @brief Construye el inspector sobre el motor de analisis y el almacen
     *        de documentos del servidor.
     * @param engine Motor de analisis (cache del CompileResult).  Debe vivir
     *               mas que el inspector.
     * @param docs   Almacen de documentos abiertos.  Debe vivir mas que el
     *               inspector.
     */
    Inspector(AnalysisEngine &engine, DocumentStore &docs) noexcept;

    ~Inspector();

    Inspector(const Inspector &) = delete;
    Inspector &operator=(const Inspector &) = delete;

    /**
     * @brief @c vesta/bytecode: texto @c .vel del modulo.
     * @param uri URI del documento abierto.
     * @return @c { "text": "<bytecode .vel>" } o @c { "error": "..." }.
     */
    nlohmann::json bytecode(const std::string &uri);

    /**
     * @brief @c vesta/ir: SSA IR del modulo.
     * @param uri   URI del documento.
     * @param phase "pre" (antes de optimizar) o "post" (default, optimizado).
     * @return @c { "text": "<IR legible>" } o @c { "error": "..." }.
     */
    nlohmann::json ir(const std::string &uri, const std::string &phase);

    /**
     * @brief @c vesta/complexity: coste Big-O por funcion.
     * @param uri URI del documento.
     * @return @c { "functions": [ { name, partial, total, confidence,
     *         total_confidence, max_loop_depth, recursive, declared,
     *         contract_mismatch }, ... ] } o @c { "error": "..." }.
     */
    nlohmann::json complexity(const std::string &uri);

    /**
     * @brief @c vesta/diagram: diagrama de una fase en un formato.
     * @param uri    URI del documento.
     * @param kind   "ast" | "ir-pre" | "ir-post" | "vel".
     * @param format "mermaid" | "graphviz" | "html".
     * @param cost   Si true, anota los nodos-funcion con su coste Big-O.
     * @return @c { "text": "<diagrama>" } o @c { "error": "..." }.
     */
    nlohmann::json diagram(const std::string &uri, const std::string &kind,
                           const std::string &format, bool cost);

    /**
     * @brief @c vesta/functions: lista de funciones del modulo.
     * @param uri URI del documento.
     * @return @c { "functions": [ { name, line }, ... ] } o
     *         @c { "error": "..." }.
     */
    nlohmann::json functions(const std::string &uri);

    /**
     * @brief @c vesta/aotCompat: reporte de compatibilidad AOT.
     * @param uri  URI del documento.
     * @param tier "bare" (default) | "embed" | "full".
     * @return @c { "compatible": bool, "issues": [ { fn_name, source_line,
     *         op, reason } ], "ok_functions": [ ... ] } o @c { "error": ... }.
     */
    nlohmann::json aot_compat(const std::string &uri, const std::string &tier);

    /**
     * @brief @c vesta/jitAsm: desensamblado x86-64 del codigo JIT de una
     *        funcion.
     * @param uri      URI del documento.
     * @param function Nombre de la funcion a compilar (vacio = la primera /
     *                 @c main si existe).
     * @return En exito @c { "text": "<disasm>", "function": "<name>",
     *         "bytes": N, "instructions": M }.  Si la funcion usa ops no
     *         soportadas por el selector JIT @c { "unsupported": true,
     *         "reason": "..." }.  Ante otro fallo @c { "error": "..." }.
     */
    nlohmann::json jit_asm(const std::string &uri, const std::string &function);

    /**
     * @brief @c vesta/aotAsm: desensamblado x86-64 del codigo AOT de una
     *        funcion.
     * @param uri      URI del documento.
     * @param function Nombre de la funcion (vacio = la primera / @c main).
     * @return En exito @c { "text": "<disasm>", "function": "<name>",
     *         "bytes": N, "relocs": [ { offset, kind, symbol, addend } ] }.
     *         Si la funcion no es AOT-compatible @c { "incompatible": true,
     *         "reason": "..." }.  Ante otro fallo @c { "error": "..." }.
     */
    nlohmann::json aot_asm(const std::string &uri, const std::string &function);

    /**
     * @brief @c vesta/macroExpand: codigo que generan los @Macro del modulo.
     *
     * Lee las expectaciones de @Macro capturadas por el TypeChecker
     * (@c CompileResult::macro_expectations, ya cacheadas por el motor: no
     * requiere flag ni recompilar).  Cada entrada describe un call site con
     * su nombre, args y el codigo Vex generado por la expansion.  Incluye
     * tambien los @Macro que NO se pudieron expandir
     * (@c macro_skip_reasons) y por que.
     *
     * @param uri URI del documento abierto.
     * @return @c { "expansions": [ { macro_name, call_site_loc, args:[...],
     *         generated_code } ], "skipped": [ { name, reason } ] } o
     *         @c { "error": "..." }.  Listas vacias si no hay macros.
     */
    nlohmann::json macro_expand(const std::string &uri);

    /**
     * @brief @c vesta/comptimeValues: valores @c comptime computados.
     *
     * Recompila el documento con @c CompileOptions::dump_comptime_values
     * (vista on-demand: NO se hace en el analyze por pulsacion) y cachea el
     * JSON por (uri, hash) en @c view_cache_.  Devuelve el snapshot de las
     * constantes @c comptime top-level que el compilador resolvio.
     *
     * @param uri URI del documento abierto.
     * @return @c { "values": [ { name, scope, type_kind, value_str } ] } o
     *         @c { "error": "..." }.  Lista vacia si no hay valores comptime.
     */
    nlohmann::json comptime_values(const std::string &uri);

  private:
    /// Estado opaco del subsistema JIT propio (CodeCache + RuntimeEntries +
    /// JitCompiler).  Inicializado perezosamente en la primera @c jit_asm.
    struct JitState;

    /// Inicializa (si hace falta) y devuelve el estado JIT, o nullptr si la
    /// inicializacion fallo.  Idempotente.
    JitState *jit_state();

    AnalysisEngine &engine_;  ///< Motor de analisis (cache CompileResult).
    DocumentStore &docs_;     ///< Almacen de documentos abiertos.

    std::unique_ptr<JitState> jit_;       ///< Subsistema JIT (lazy).
    bool jit_init_failed_ = false;        ///< true si la init del JIT fallo.

    /// Cache propia de vistas caras (diagramas + ir-pre) por clave compuesta
    /// "<uri>|<hash>|<vista>".  Evita recompilar peticiones identicas.
    std::unordered_map<std::string, std::string> view_cache_;
};

} // namespace lsp

#endif // VESTA_LSP_INSPECTOR_H
