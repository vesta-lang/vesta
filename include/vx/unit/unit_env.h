/**
 * @file unit_env.h
 * @brief Lo que necesita compilar UN modulo de un proyecto, y lo que produce.
 *
 * La compilacion de cada modulo era una lambda de mil doscientas lineas que
 * capturaba todo el entorno de `compile_vx_project` por referencia: no se
 * podia leer por partes, ni probar, ni reutilizar desde el camino de fichero
 * suelto.  Aqui ese entorno tiene nombre y esta agrupado por lo que es -- la
 * cache, lo que el raiz teje en todos, lo que se comparte entre modulos --,
 * y cada pieza de la compilacion de un modulo recibe esto en vez de un cierre.
 *
 * Los punteros no son propietarios: todo vive en `compile_vx_project`, que
 * dura mas que cualquier compilacion de un modulo.
 */
#ifndef VX_UNIT_UNIT_ENV_H
#define VX_UNIT_UNIT_ENV_H

#include "util/name_pool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vx {

struct CompileOptions;
struct CompileResult;
struct ProjectModuleWork;
struct ModuleLookup;
class CasStore;
class GenericInstanceRegistry;
namespace ast {
struct FunctionDecl;
} // namespace ast

/// Que decide si un artefacto guardado sirve, y donde se guarda.
struct UnitCacheEnv {
    bool enabled = false; ///< la cache junto al fuente esta encendida
    bool verbose = false; ///< contar aciertos y fallos por la salida de error
    /// La cache comun por contenido (`VX_CAS_DIR`); nulo si esta apagada.
    CasStore *cas = nullptr;
    /// Huella de la configuracion que cambia el intermedio antes de optimizar.
    uint64_t cas_config_fp = 0;
    /// Huella de lo que tejen los `@Hook` del raiz en los demas modulos.
    uint64_t hooks_source_fp = 0;
    /// El objetivo de `@Target` capturado en el hilo principal.
    const std::string *target_os = nullptr;
    const std::string *target_arch = nullptr;
};

/// Un `@Hook` del raiz y el nombre por el que se le llama desde fuera.
using RootHook = std::pair<ast::FunctionDecl *, std::string>;
/// Contador de instalaciones de un gancho, compartido por todos los modulos.
using HookCounters =
    std::unordered_map<std::string, std::shared_ptr<std::atomic<size_t>>>;

/// Lo que el modulo RAIZ decide para todo el programa.
struct RootWeavingEnv {
    /// `@HelperOverride` ya resueltos por precedencia: destino -> simbolo.
    const std::unordered_map<std::string, std::string> *helper_overrides =
        nullptr;
    /// Los `@Hook` del raiz, que se tejen en todos los demas modulos.
    const std::vector<RootHook> *hooks = nullptr;
    /// Las funciones del raiz marcadas para no instrumentarse.
    const std::vector<std::string> *no_instrument = nullptr;
    /// Cuantas veces se instalo cada gancho, sumando todos los modulos.
    const HookCounters *hook_counters = nullptr;
};

/// Todo lo que la compilacion de un modulo lee del proyecto.
struct UnitEnv {
    /// Los modulos del proyecto, en orden topologico.  Cada compilacion solo
    /// ESCRIBE en el suyo; de los demas lee los ya terminados.
    std::vector<ProjectModuleWork> *work = nullptr;
    /// Donde esta cada modulo y como se resuelve un import.
    const ModuleLookup *lookup = nullptr;
    /// El nivel topologico de cada modulo, para el progreso.
    const std::vector<int> *levels = nullptr;

    const CompileOptions *opts = nullptr;         ///< las del proyecto
    const CompileOptions *opts_modules = nullptr; ///< las que ven los modulos

    UnitCacheEnv cache;
    RootWeavingEnv root;

    /// Reparto de instanciaciones genericas entre modulos; nulo si cada uno
    /// produce las suyas.
    GenericInstanceRegistry *generic_instances = nullptr;
    /// Lo que el parser dejo fuera por `@Target`, de todos los modulos.
    const std::unordered_map<std::string, std::vector<std::string>>
        *target_skipped = nullptr;
    /// Identidad de paquete: la del proyecto y la que un modulo declare.
    const std::string *project_package_id = nullptr;
    const std::vector<std::string> *module_package_override = nullptr;

    bool verbose_compile = false; ///< una linea por modulo al empezar
    /// Serializa lo que varios hilos escriben por la salida de error.
    std::mutex *verbose_mtx = nullptr;

    /// El resultado del proyecto.  Solo lo toca el RAIZ, que se compila solo
    /// en el ultimo nivel, asi que no compite con nadie.
    CompileResult *res = nullptr;
    /// La identidad de la cache de hechos del raiz; la escribe su tarea.
    uint64_t *root_facts_key = nullptr;
};

/// Las claves de cache de un modulo, calculadas una vez y usadas al buscarlo
/// y al guardarlo.
struct UnitCacheKeys {
    uint64_t source_hash = 0;         ///< contenido + configuracion
    util::InternedName target_suffix; ///< separa por objetivo
    uint64_t cas_key = 0;             ///< clave en la cache comun
    bool cas_key_ok = false;          ///< si hay clave comun
};

} // namespace vx

#endif // VX_UNIT_UNIT_ENV_H
