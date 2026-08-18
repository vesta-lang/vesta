/**
 * @file semantic_index.h
 * @brief Indice semantico por-declaracion: hash de contenido + grafo de
 *        dependencias.  Sustrato de la compilacion incremental granular
 *        (por-simbolo) y, a futuro, de la compilacion DISTRIBUIDA.
 *
 * Motivacion: hoy la cache del compilador invalida por MoDULO completo
 * (@c source_hash = fnv1a del fichero entero).  Cambiar un byte recompila
 * todo el modulo.  Este indice baja la granularidad a la DECLARACIoN: cada
 * simbolo (fn/struct/class/enum/concept/global/typedef) lleva un hash de
 * contenido estable frente a su POSICIoN y frente a cambios en OTRAS decls,
 * mas la lista de simbolos que referencia.  Con eso, un rebuild solo
 * recompila el conjunto de simbolos cuyo hash cambio MAS su cierre
 * transitivo por dependencias, reutilizando el resto.
 *
 * Por que habilita compilacion DISTRIBUIDA: las claves son content-hashes
 * (mismo input -> mismo hash -> mismo artefacto), asi un almacen
 * direccionado por contenido (CAS) remoto puede servir el artefacto de
 * cualquier simbolo compilado en CUALQUIER nodo; el grafo de dependencias
 * permite repartir simbolos independientes entre maquinas.  La granularidad
 * por-fichero no se puede distribuir fino.
 */
#ifndef VESTA_VX_SEMANTIC_INDEX_H
#define VESTA_VX_SEMANTIC_INDEX_H

#include <cstdint>
#include <string>
#include <vector>

#include "vx/ast.h"

namespace vx {

/**
 * @struct SymbolEntry
 * @brief Una declaracion top-level indexada.
 */
struct SymbolEntry {
    std::string name; ///< nombre CUALIFICADO con el namespace (punteado).
    uint8_t kind = 0; ///< @c ast::NodeKind del simbolo (como u8).
    uint64_t content_hash = 0; ///< FNV-1a 64 del span de fuente del simbolo.
    std::vector<std::string> deps; ///< nombres SIMPLES de simbolos referidos.
    uint32_t src_offset = 0;       ///< offset del span en la fuente original.
    uint32_t src_length = 0;       ///< longitud del span en bytes.
    bool is_public = true; ///< @c true si la decl es @c public (importable).
};

/**
 * @struct SemanticIndex
 * @brief Indice por-declaracion de un modulo.
 */
struct SemanticIndex {
    std::string module_path;  ///< path canonico del modulo (identidad).
    uint64_t module_hash = 0; ///< FNV-1a del fuente completo (compat cache).
    std::vector<SymbolEntry> symbols;

    /// @brief Busca un simbolo por nombre cualificado (nullptr si no existe).
    const SymbolEntry *find(const std::string &qualified_name) const;
};

/**
 * @brief Construye el indice desde el AST RAW (PRE-flatten de namespaces) y
 *        la fuente original.
 *
 * Debe llamarse ANTES del aplanado de namespaces: los offsets de las decls
 * apuntan a la fuente tal como la escribio el usuario, y el hash de contenido
 * es sobre ese texto.  Los nombres se cualifican con el path del namespace
 * contenedor.
 *
 * @param mod         Raiz del AST parseado.
 * @param source      Texto fuente original completo.
 * @param module_path Identidad del modulo (path canonico).
 * @return Indice semantico del modulo.
 */
SemanticIndex build_semantic_index(const ast::ModuleNode &mod,
                                   const std::string &source,
                                   const std::string &module_path);

/**
 * @brief Serializa el indice a bytes (sidecar @c .vxidx, versionado).
 */
std::vector<uint8_t> serialize_semantic_index(const SemanticIndex &idx);

/**
 * @brief Deserializa un indice desde bytes.  @return false si el formato es
 *        invalido (magic/version).
 */
bool parse_semantic_index(const std::vector<uint8_t> &bytes,
                          SemanticIndex &out);

/**
 * @brief Conjunto de simbolos a RECOMPILAR entre dos versiones del indice:
 *        los que cambiaron de hash, aparecieron o desaparecieron, MAS el
 *        cierre transitivo de sus DEPENDIENTES (quien los referencia).
 *
 * Es la operacion central del driver incremental: dado el indice previo
 * (cacheado) y el nuevo (tras editar), devuelve exactamente que simbolos hay
 * que rehacer; el resto se sirve de cache.
 *
 * @param old_idx Indice previo (cacheado).
 * @param new_idx Indice nuevo (tras la edicion).
 * @return Nombres cualificados de los simbolos a recompilar (orden estable).
 */
std::vector<std::string> changed_symbols_closure(const SemanticIndex &old_idx,
                                                 const SemanticIndex &new_idx);

/**
 * @brief Volcado JSON del indice (depuracion, flag @c --dump-semantic-index).
 */
std::string semantic_index_to_json(const SemanticIndex &idx);

/**
 * @struct ImportedModuleSemIndex
 * @brief Indice semantico de un modulo IMPORTADO por el fichero analizado,
 *        con su fuente cruda (para extraer firmas) y su uri.
 *
 * Lo consume el LSP para el completado / navegacion CROSS-MODULE: cuando el
 * usuario escribe @c "lib.<TAB>", los simbolos publicos de @c lib viven en
 * OTRO fichero, no en el indice del documento actual.
 */
struct ImportedModuleSemIndex {
    std::string uri; ///< file:// del modulo importado (para Location futura).
    std::string source; ///< fuente cruda del modulo (extraccion de firmas).
    SemanticIndex index;
};

/**
 * @brief Construye los indices semanticos de todos los modulos IMPORTADOS
 *        (transitivamente) por @p root_file, EXCLUYENDO el propio root.
 *
 * Reusa @c ModuleGraph (misma resolucion de paths que el compilador: dir del
 * importer, search paths, VX_PATH, stdlib) para encontrar cada dependencia,
 * la parsea y construye su @c SemanticIndex.  Best-effort: los modulos que no
 * resuelvan o no parseen simplemente se omiten (no aborta).
 *
 * @param root_file          Path del fichero raiz (el que se esta editando).
 * @param root_overlay_text  Texto en memoria del root (buffer del editor).
 * @param extra_search_paths Search paths extra (ancestros del root, etc.).
 * @return Un indice por modulo importado alcanzable.
 */
std::vector<ImportedModuleSemIndex>
build_imported_sem_indexes(const std::string &root_file,
                           const std::string &root_overlay_text,
                           const std::vector<std::string> &extra_search_paths);

} // namespace vx

#endif // VESTA_VX_SEMANTIC_INDEX_H
