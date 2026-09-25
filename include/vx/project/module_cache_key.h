/**
 * @file module_cache_key.h
 * @brief La IDENTIDAD en cache del artefacto de un modulo: todo lo que decide
 *        si un `.vxi`/`.vxir` guardado sirve para esta compilacion.
 *
 * Estaba escrito dentro de la lambda que compila cada modulo del proyecto,
 * mezclado con la compilacion misma.  Es una responsabilidad aparte y la mas
 * delicada de las dos: lo que cambia el artefacto y NO entra en la clave hace
 * que un acierto de cache sirva algo que no corresponde, y eso no da un error,
 * da otro programa.  Cada ingrediente de abajo entro despues de morder asi.
 */
#ifndef VX_PROJECT_MODULE_CACHE_KEY_H
#define VX_PROJECT_MODULE_CACHE_KEY_H

#include "util/name_pool.h" // util::InternedName
#include "util/named_alloc.h"

#include <cstdint>
#include <string>

namespace vx {

struct CompileOptions;

namespace scratch {
struct DepAbiHashes; ///< Huellas de interfaz de las dependencias de un modulo.
} // namespace scratch

/// Las huellas de interfaz (`abi_hash`) de las dependencias directas de un
/// modulo, ORDENADAS: lo que de ellas entra en su clave de contenido.
using DepAbiHashes = util::NamedVector<uint64_t, scratch::DepAbiHashes>;

/**
 * @brief Huella de ESTA construccion del compilador.
 *
 * El compilador forma parte de lo que produjo un artefacto: el mismo fuente
 * compilado por dos versiones distintas da IR distinto.  Sale del propio
 * ejecutable (tamano y fecha), asi que cambia en cuanto se recompila el
 * compilador; `VX_CACHE_FINGERPRINT` la fija para depurar sin perder la cache
 * que reproduce un fallo.  Se calcula una sola vez.
 *
 * @return Valor que identifica esta version del compilador.
 */
uint64_t compiler_fingerprint();

/// Lo que decide la identidad del artefacto de un modulo, aparte de su fuente.
struct ModuleCacheKeyInput {
    /// El texto del modulo.
    const std::string *source = nullptr;
    /// Las opciones con que se compila.
    const CompileOptions *opts = nullptr;
    /// Si hay maquina de compilacion cargada: con ella el cuerpo de un `asm`
    /// generado por una funcion comptime sale escrito, sin ella vacio.
    bool comptime_machine = false;
    /// Huella de los `@Hook` del raiz, que tejen llamadas en este modulo.
    uint64_t hooks_source_fp = 0;
    /// Sistema operativo y arquitectura del objetivo de `@Target`.
    const std::string *target_os = nullptr;
    const std::string *target_arch = nullptr;
};

/// La identidad resultante.
struct ModuleCacheKey {
    /// Clave del contenido y de todo lo que cambia lo compilado.
    uint64_t source_hash = 0;
    /// Sufijo del fichero de cache, INTERNADO: separa por objetivo los modulos
    /// que usan `@Target` y marca la pasada con maquina de compilacion.  El
    /// nombre vacio en el caso normal.
    util::InternedName target_suffix;
};

/**
 * @brief Calcula la identidad en cache del artefacto de un modulo.
 * @param in Los ingredientes.
 * @return La clave y el sufijo del fichero.
 */
ModuleCacheKey module_cache_key(const ModuleCacheKeyInput &in);

/**
 * @brief Clave de CONTENIDO de un modulo para el almacen comun (CAS).
 *
 * A diferencia de la cache por ruta, esta clave no depende de DONDE esta el
 * modulo: solo de su contenido, de las huellas de interfaz de sus dependencias
 * directas, de la version del compilador y del objetivo.  Asi dos proyectos con
 * la MISMA stdlib en rutas distintas obtienen la misma clave y la compilan una
 * sola vez para toda la maquina; es tambien la base de la compilacion
 * distribuida (misma clave, mismo artefacto en cualquier nodo).  Es ademas la
 * identidad con la que se guardan los hechos del modulo raiz.
 *
 * @param source_hash   La de @ref module_cache_key.
 * @param dep_hashes    Huellas de interfaz de las dependencias, ORDENADAS.
 * @param target_suffix El sufijo de @ref module_cache_key.
 * @param config_fp     Huella de la configuracion que cambia el IR.
 * @return La clave.
 */
uint64_t module_content_key(uint64_t source_hash,
                            const DepAbiHashes &dep_hashes,
                            const std::string &target_suffix,
                            uint64_t config_fp);

} // namespace vx

#endif // VX_PROJECT_MODULE_CACHE_KEY_H
