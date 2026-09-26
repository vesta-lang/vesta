/**
 * @file compile_unit.h
 * @brief Compilar UN modulo de un proyecto, y las piezas en que se reparte.
 *
 * Era la lambda `compile_one_module` de `compile_vx_project`: mil doscientas
 * lineas que capturaban todo el entorno por referencia.  Aqui cada paso tiene
 * nombre y fichero -- la cache, la preparacion del AST, los imports, la
 * comprobacion de tipos, la bajada, la interfaz --, y todos reciben el entorno
 * por @ref vx::UnitEnv en vez de por un cierre.
 *
 * El ORDEN de las piezas es el de la lambda, y lo fija @ref vx::compile_unit.
 * Cada pieza que puede abortar la compilacion del modulo devuelve @c false, y
 * entonces quien la llama marca el modulo como fallido y no sigue.
 */
#ifndef VX_UNIT_COMPILE_UNIT_H
#define VX_UNIT_COMPILE_UNIT_H

#include "vx/module/namespace_flatten.h" // FlattenedNamespace
#include "vx/project/module_imports.h"   // ImportRequest
#include "vx/unit/unit_env.h"

#include <cstddef>
#include <string>
#include <vector>

namespace vx {

/**
 * @brief Compila el modulo @p i del proyecto, o lo sirve de la cache.
 *
 * Deja el resultado en `(*env.work)[i]`: su IR, su interfaz y si compilo.
 *
 * @param env El entorno del proyecto.
 * @param i   Indice del modulo, en orden topologico.
 */
void compile_unit(const UnitEnv &env, size_t i);

/**
 * @brief Calcula las claves de cache del modulo @p i.
 *
 * Si el modulo es el RAIZ, ademas deja su identidad en `*env.root_facts_key`.
 *
 * @param env El entorno del proyecto.
 * @param i   Indice del modulo.
 * @return Las claves, para buscarlo y para guardarlo.
 */
UnitCacheKeys unit_cache_keys(const UnitEnv &env, size_t i);

/**
 * @brief Intenta servir el modulo @p i de la cache comun o de la cache junto
 *        al fuente.
 * @param env  El entorno del proyecto.
 * @param i    Indice del modulo.
 * @param keys Sus claves de cache.
 * @return @c true si quedo servido y no hay que compilarlo.
 */
bool serve_unit_from_cache(const UnitEnv &env, size_t i,
                           const UnitCacheKeys &keys);

/**
 * @brief Guarda la interfaz y el IR del modulo @p i para la proxima vez.
 * @param env  El entorno del proyecto.
 * @param i    Indice del modulo.
 * @param keys Sus claves de cache.
 */
void persist_unit(const UnitEnv &env, size_t i, const UnitCacheKeys &keys);

/**
 * @brief Prepara el AST del modulo @p i antes de comprobar tipos: el prefijo
 *        de modulo, el aplanado de namespaces y el conjunto comptime.
 * @param env El entorno del proyecto.
 * @param i   Indice del modulo.
 * @return Los namespaces inline aplanados, que el comprobador de tipos
 *         necesita registrar.
 */
std::vector<FlattenedNamespace> prepare_unit(const UnitEnv &env, size_t i);

/**
 * @brief Crea el comprobador de tipos del modulo @p i, lo configura e inyecta
 *        en el lo que importa.
 * @param env               El entorno del proyecto.
 * @param i                 Indice del modulo.
 * @param inline_namespaces Los namespaces propios, de @ref prepare_unit.
 * @param imports           [out] Los imports del modulo, que los pasos de
 *                          despues vuelven a necesitar.
 * @return @c false si el modulo no puede seguir compilandose.
 */
bool inject_unit_imports(const UnitEnv &env, size_t i,
                         const std::vector<FlattenedNamespace> &inline_namespaces,
                         std::vector<ImportRequest> &imports);

/**
 * @brief Comprueba los tipos del modulo @p i y, si es el raiz, avisa de los
 *        imports que no usa.
 * @param env     El entorno del proyecto.
 * @param i       Indice del modulo.
 * @param imports Sus imports.
 * @return @c false si la comprobacion fallo.
 */
bool check_unit(const UnitEnv &env, size_t i,
                const std::vector<ImportRequest> &imports);

/**
 * @brief Baja el modulo @p i a IR y emite su grafo de depuracion.
 * @param env El entorno del proyecto.
 * @param i   Indice del modulo.
 * @return @c false si la bajada fallo.
 */
bool lower_unit(const UnitEnv &env, size_t i);

/**
 * @brief Construye la interfaz (`.vxi`) del modulo @p i ya compilado.
 * @param env     El entorno del proyecto.
 * @param i       Indice del modulo.
 * @param keys    Sus claves de cache (la huella del fuente va en la interfaz).
 * @param imports Sus imports, para la tabla de dependencias.
 */
void build_unit_interface(const UnitEnv &env, size_t i,
                          const UnitCacheKeys &keys,
                          const std::vector<ImportRequest> &imports);

/**
 * @brief La identidad de paquete del modulo @p i: la que declare el propio
 *        modulo, o la del proyecto si no declara ninguna.
 * @param env El entorno del proyecto.
 * @param i   Indice del modulo.
 * @return El PackageId (puede ser vacio).
 */
std::string unit_package_id(const UnitEnv &env, size_t i);

} // namespace vx

#endif // VX_UNIT_COMPILE_UNIT_H
