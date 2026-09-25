/**
 * @file module_paths.h
 * @brief DONDE van los ficheros de un modulo: sus artefactos en la cache y sus
 *        volcados de depuracion.
 *
 * Como se forma una ruta -- el cajon, la huella de la ruta canonica delante,
 * el nombre del modulo detras, la extension -- es una representacion, y vive
 * aqui para que nadie mas la conozca.  Construida a mano en cada sitio, dos
 * volcados ya se pisaban (dos `x86_64.vx` distintos daban el mismo nombre).
 */
#ifndef VX_PROJECT_MODULE_PATHS_H
#define VX_PROJECT_MODULE_PATHS_H

#include "util/name_pool.h" // util::InternedName

#include <string>

namespace vx {

/// Las rutas de cache de un modulo.
struct ModuleCachePaths {
    std::string vxi;   ///< su interfaz binaria.
    std::string vxir;  ///< su intermedio serializado.
    std::string vel;   ///< su `.vel` suelto, para distribuirlo aparte.
    std::string facts; ///< lo que el ASA supo de el al bajarlo.
};

/**
 * @brief Las rutas de cache de un modulo, calculadas la primera vez que se
 *        piden y reutilizadas despues.
 *
 * Todas salen del mismo trabajo -- mirar el cajon, hashear la ruta, componer el
 * nombre --, y cada compilacion pregunta por las mismas al menos dos veces (al
 * buscar un acierto y al escribir).  Se piden con los dos argumentos
 * INTERNADOS: la identidad de la entrada es el par de punteros.
 *
 * @param canonical_path Ruta canonica del modulo, internada.
 * @param target_suffix  Sufijo de objetivo (ver `ModuleCacheKey`), internado.
 * @return Las rutas.  La referencia vive lo que el proceso.
 */
const ModuleCachePaths &module_cache_paths(util::InternedName canonical_path,
                                           util::InternedName target_suffix);

/**
 * @brief La ruta de un volcado de depuracion de un modulo.
 *
 * Lleva la huella de la ruta canonica en el nombre: el nombre de modulo es el
 * ultimo segmento de la ruta, y dos modulos distintos pueden llamarse igual.
 *
 * @param dir            Directorio del volcado.
 * @param prefix         Que se vuelca (va delante del nombre); puede ir vacio.
 * @param module_name    Nombre del modulo.
 * @param canonical_path Su ruta canonica.
 * @param extension      Extension del fichero, con su punto.
 * @return La ruta.
 */
std::string module_dump_path(const std::string &dir, const std::string &prefix,
                             const std::string &module_name,
                             const std::string &canonical_path,
                             const std::string &extension);

} // namespace vx

#endif // VX_PROJECT_MODULE_PATHS_H
