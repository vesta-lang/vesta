/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file analysis_engine.h
 * @brief Motor de analisis de documentos Vesta reutilizable por el LSP.
 *
 * Punto UNICO por el que pasan todas las peticiones del servidor que
 * necesitan compilar/analizar un documento.  En esta Fase 1 produce
 * diagnosticos en vivo; en fases posteriores las peticiones de IR,
 * bytecode, JIT, AOT, complejidad y diagramas reutilizaran el
 * @c CompileResult cacheado que aqui se guarda, sin recompilar.
 *
 * Diseno:
 *  - @c analyze_document(uri, text) compila el fuente con
 *    @c vx::compile_vx_source envuelto en try/catch para que un fallo
 *    del frontend NO tumbe el servidor.
 *  - Cachea por (uri, hash del texto): si el texto no cambio, devuelve el
 *    analisis previo sin recompilar.  Esto evita recompilaciones en
 *    peticiones encadenadas (diagnosticos + hover + tokens sobre el mismo
 *    estado del documento).
 *  - Expone el @c CompileResult completo para que futuras fases extraigan
 *    vel_text / ir_text / diagramas / IR serializado, etc.
 */

#ifndef VESTA_LSP_ANALYSIS_ENGINE_H
#define VESTA_LSP_ANALYSIS_ENGINE_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "analyze/bigo.h"
#include "vx/compiler.h"
#include "vx/semantic_index.h"

namespace lsp {

/**
 * @struct DocAnalysis
 * @brief Resultado del analisis de un documento (cacheable y reutilizable).
 *
 * Guarda el @c CompileResult completo (diagnosticos, vel/ir text,
 * diagramas, IR serializado) ademas del hash del texto con el que se
 * produjo, para invalidar la cache cuando el documento cambia.
 */
struct DocAnalysis {
    uint64_t text_hash = 0;       ///< Hash FNV-1a del texto analizado.
    vx::CompileResult result;    ///< Resultado completo de la compilacion.

    /// Nombres de clases declaradas top-level (para enriquecer el resaltado
    /// semantico: un IDENTIFIER que coincide se clasifica como @c class).
    std::unordered_set<std::string> class_names;
    /// Nombres de structs declarados top-level (clasificados como @c struct).
    std::unordered_set<std::string> struct_names;
    /// Nombres de enums declarados top-level (clasificados como @c enum).
    std::unordered_set<std::string> enum_names;
    /// Nombres de alias de tipo (typedef/using) declarados (clasificados
    /// como @c type).
    std::unordered_set<std::string> type_names;
    /// Nombres de funciones declaradas top-level (clasificadas como
    /// @c function).
    std::unordered_set<std::string> function_names;
    /// Nombres de parametros de plantilla (genericos) declarados en el
    /// documento: @c class Box<T>, @c comptime <T> u32 f(), enums
    /// genericos, etc.  Un IDENTIFIER que coincide se clasifica como
    /// @c typeParameter.  v1: set GLOBAL del documento (sin scope por
    /// declaracion), trade-off aceptable: si un nombre de type-param
    /// coincide con otro uso, se coloreara como typeParameter.
    std::unordered_set<std::string> type_params;

    /// Coste/complejidad por funcion del modulo (PARCIAL + TOTAL
    /// interprocedural), computado una sola vez sobre el IR post-opt cacheado.
    /// Lo consume el hover para mostrar la complejidad Big-O de una funcion.
    /// Vacio si el fuente no produjo IR (errores de compilacion).
    analyze::ModuleCost cost;

    /// Indice semantico por-declaracion (NS.4): nombres CUALIFICADOS por
    /// namespace + hash + deps.  Se construye del AST RAW (pre-flatten) para
    /// que el LSP pueda ofrecer completado de miembro de namespace
    /// (`ns.simbolo`), resolver acceso cualificado, etc.  Vacio si el parse
    /// fallo.
    vx::SemanticIndex sem_index;

    /// Indices semanticos de los modulos IMPORTADOS por este documento (cada
    /// uno con su fuente y uri), para completado / navegacion CROSS-MODULE:
    /// `import "lib"; lib.<TAB>` ofrece los simbolos publicos de @c lib aunque
    /// vivan en otro fichero.  Vacio si el documento no importa nada.
    std::vector<vx::ImportedModuleSemIndex> imported_sem_indexes;
};

/**
 * @class AnalysisEngine
 * @brief Compila y cachea analisis de documentos Vesta para el LSP.
 *
 * No es thread-safe: el servidor procesa peticiones secuencialmente.
 */
class AnalysisEngine {
  public:
    /**
     * @brief Analiza un documento, reutilizando la cache si el texto no
     *        cambio.
     *
     * Compila @p text con @c vx::compile_vx_source bajo try/catch.  Si la
     * compilacion lanza una excepcion (fuente parcial/invalida mientras se
     * teclea), devuelve un @c DocAnalysis con un unico diagnostico de error
     * interno en la posicion 0:0 en lugar de propagar el fallo.
     *
     * @param uri  URI del documento (para nombrar el fichero en diagnosticos
     *             y como clave de cache).
     * @param text Texto completo actual del documento.
     * @return Referencia al @c DocAnalysis cacheado para este documento.  La
     *         referencia es valida hasta el siguiente @c analyze_document
     *         sobre el MISMO uri o hasta @c forget.
     */
    const DocAnalysis &analyze_document(const std::string &uri,
                                        const std::string &text);

    /**
     * @brief Devuelve el analisis cacheado de un documento, o nullptr.
     * @param uri URI del documento.
     * @return Puntero al analisis previo, o nullptr si no hay ninguno.
     */
    const DocAnalysis *cached(const std::string &uri) const;

    /**
     * @brief Olvida el analisis cacheado de un documento (al cerrarlo).
     * @param uri URI del documento.
     */
    void forget(const std::string &uri);

  private:
    /// Cache por URI del ultimo analisis (uno por documento abierto).
    std::unordered_map<std::string, std::unique_ptr<DocAnalysis>> cache_;
};

/**
 * @brief Hash FNV-1a 64-bit de una cadena (helper de la cache).
 * @param s Cadena a hashear.
 * @return Hash de 64 bits.
 */
uint64_t fnv1a_hash(const std::string &s);

} // namespace lsp

#endif // VESTA_LSP_ANALYSIS_ENGINE_H
