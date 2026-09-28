/**
 * @file module_paths.cpp
 * @brief Donde van los ficheros de un modulo.
 * @see vx/project/module_paths.h
 */
#include "vx/project/module_paths.h"

#include "util/cache_paths.h" // el reparto de la cache por tipo y alcance
#include "util/fnv.h"

#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace vx {

namespace {

/* Donde caen los artefactos de un modulo.
 *
 * En la cache, SIEMPRE, y en el cajon de su tipo -- lo reparte
 * `util/cache_paths.h` --.  Antes caian junto al fuente (`lib.vx` -> `lib.vxi`)
 * salvo que se pusiera `VX_CACHE_DIR`, y eso costaba de tres maneras:
 * ensuciaba el arbol de fuentes con ficheros que salian en `git status` sin que
 * nadie supiera si hacian falta; obligaba a limpiar por lista de extensiones --
 * borrar `.cache` dejaba vivos los `.vxi`, y quien media en frio media en
 * caliente sin enterarse --; y hacia que el mismo proyecto tuviera dos
 * disposiciones distintas segun una variable de entorno, o sea dos caminos que
 * probar.
 *
 * Lo que motivaba tenerlos al lado -- publicar una libreria copiando el `.vx`
 * con su `.vxi` -- no se pierde: son artefactos PORTABLES y siguen juntos, en
 * `.cache/ir`.  Copiar ese cajon es exactamente lo mismo, y ademas se puede
 * hacer de golpe.  @see util::CacheScope */

/**
 * @brief El nombre de fichero de un artefacto de @p source_path.
 *
 * Lleva la huella de la ruta CANONICA delante para que dos modulos con el mismo
 * nombre de fichero -- que en un proyecto con varias carpetas es lo normal --
 * no se pisen ahora que todos comparten cajon; y el nombre del modulo detras,
 * para que mirando el directorio se pueda saber de que es cada uno.
 *
 * @param source_path Ruta canonica del modulo.
 * @param tail        Lo que va detras del nombre: objetivo y extension.
 * @return Nombre de hoja, sin directorio.
 */
std::string cache_file_name(const std::string &source_path,
                            const std::string &tail) {
    return module_path_tag(source_path) + "_" +
           std::filesystem::path(source_path).stem().string() + tail;
}

/**
 * @brief Donde va un artefacto de @p source_path del tipo @p kind.
 * @param source_path Ruta canonica del modulo.
 * @param kind        Que tipo de artefacto es; decide el cajon.
 * @param tail        Objetivo y extension.
 * @return Ruta completa dentro de la cache.
 */
std::string cache_path(const std::string &source_path, util::CacheKind kind,
                       const std::string &tail) {
    return (std::filesystem::path(util::cache_dir(kind)) /
            cache_file_name(source_path, tail))
        .string();
}

/// La identidad de una entrada de la tabla: los dos argumentos internados.
struct PathsKey {
    util::InternedName canonical_path;
    util::InternedName target_suffix;
    bool operator==(const PathsKey &o) const noexcept {
        return canonical_path == o.canonical_path &&
               target_suffix == o.target_suffix;
    }
};

/// Hash de @ref PathsKey: las dos identidades, sin mirar el texto.
struct PathsKeyHash {
    size_t operator()(const PathsKey &k) const noexcept {
        const util::InternedNameHash h;
        return static_cast<size_t>(
            util::hash_combine(h(k.canonical_path), h(k.target_suffix)));
    }
};

} // namespace

std::string module_path_tag(const std::string &canonical_path) {
    const uint64_t h = util::fnv_bytes(util::kFnvOffset, canonical_path.data(),
                                       canonical_path.size());
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(h));
    return std::string(hex);
}

const ModuleCachePaths &module_cache_paths(util::InternedName canonical_path,
                                           util::InternedName target_suffix) {
    /* Compartida entre los hilos que compilan modulos en paralelo, asi que va
     * con cerrojo.  Es un puñado de entradas y se tocan una vez por modulo: el
     * cerrojo cuesta muchisimo menos que rehacer las rutas.  Los nodos no se
     * mueven al crecer, asi que la referencia devuelta sigue valiendo. */
    static std::mutex mtx;
    static std::unordered_map<PathsKey, ModuleCachePaths, PathsKeyHash> table;
    const PathsKey key{canonical_path, target_suffix};
    std::lock_guard<std::mutex> lk(mtx);
    auto it = table.find(key);
    if (it != table.end()) return it->second;
    const std::string &src = canonical_path.str();
    const std::string &tgt = target_suffix.str();
    ModuleCachePaths p;
    /* La interfaz y el intermedio comparten cajon a proposito: son la misma
     * compilacion vista por sus dos caras, nacen y mueren juntos, y la
     * extension ya los distingue. */
    p.vxi = cache_path(src, util::CacheKind::ModuleIr, tgt + ".vxi");
    p.vxir = cache_path(src, util::CacheKind::ModuleIr, tgt + ".vxir");
    p.facts = cache_path(src, util::CacheKind::Facts, tgt + ".vxfacts");
    /* El .vel no se separa por objetivo: es el mismo modulo suelto. */
    p.vel = cache_path(src, util::CacheKind::Vel, ".vel");
    return table.emplace(key, std::move(p)).first->second;
}

std::string module_dump_path(const std::string &dir, const std::string &prefix,
                             const std::string &module_name,
                             const std::string &canonical_path,
                             const std::string &extension) {
    return dir + "/" + prefix + module_name + "_" +
           module_path_tag(canonical_path) + extension;
}

} // namespace vx
