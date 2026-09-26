/**
 * @file module_imports.h
 * @brief Que importa cada modulo del proyecto y a que modulo se refiere.
 *
 * Dos preguntas que el camino de proyecto se hace en muchos sitios -- que
 * imports tiene este modulo, y a que modulo del proyecto se refiere cada uno
 * -- y que se contestaban a mano en cada uno de ellos.  La segunda es la que
 * mas duele: un modulo se puede encontrar por su NOMBRE (el de su fichero) o
 * por su NAMESPACE, y el nombre no es unico -- `std/os/linux.vx` y
 * `std/syscall/linux.vx` son los dos `linux` --.  Quien buscaba solo por
 * nombre se llevaba el que no era: la clave de la cache, la validacion de sus
 * dependencias y el recorrido transitivo se calculaban contra OTRO modulo.
 *
 * Por eso aqui vive UNA forma de resolver un import (@ref ModuleLookup::find),
 * y las listas de modulos van por INDICE, no por nombre: un indice no se puede
 * volver a buscar mal.
 */
#ifndef VX_PROJECT_MODULE_IMPORTS_H
#define VX_PROJECT_MODULE_IMPORTS_H

#include "util/named_alloc.h"
#include "vx/ast.h"
#include "vx/diagnostic.h"
#include "vx/module/module_resolver.h" // NamespaceList
#include "vx/type_checker.h"           // TypeChecker::VxiOnlyEntry

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vx {

struct ProjectModuleWork;
struct VxiModule;

/// Un import de un modulo, ya leido de su AST.
struct ImportRequest {
    std::string module_name; ///< el modulo, por el nombre de su fichero
    std::string local_name;  ///< el alias, o el nombre del modulo
    std::vector<TypeChecker::VxiOnlyEntry> only_symbols;
    bool is_plain = false;           ///< sin `only`: registra el namespace
    bool only_all = false;           ///< `only *`: todos los publicos
    bool is_public_reexport = false; ///< `public import`
    bool by_namespace = false;       ///< `import a.b.c;` (por namespace)
    /// El namespace tal como se escribio, si el import es por namespace.  Con
    /// el se encuentran TODOS los ficheros de un namespace parcial.
    std::string ns_path;
    SourceLoc loc{}; ///< donde esta el import, para los diagnosticos
};

/// Los namespaces que el manifiesto declara auto-importables.
using AutoImportNs = NamespaceList;

/// "No hay modulo": lo que devuelve una busqueda que no encontro nada.
constexpr size_t kNoModule = SIZE_MAX;

namespace scratch {
struct ModuleIndices;
} // namespace scratch

/// Una lista de modulos del proyecto, por su indice en el trabajo.
using ModuleIndices = util::NamedVector<size_t, scratch::ModuleIndices>;

/**
 * @brief Donde esta cada modulo del proyecto, y como se resuelve un import.
 *
 * Se construye UNA vez, cuando ya estan todos los AST, con
 * @ref build_module_lookup.
 */
struct ModuleLookup {
    /// Por nombre de fichero.  NO es unico: dos ficheros homonimos colapsan
    /// y gana el primero.  Solo para lo que de verdad es un nombre (ver
    /// @ref find_by_name).
    std::unordered_map<std::string, size_t> by_name;
    /// Por namespace completo, que si es unico.
    std::unordered_map<std::string, size_t> by_ns;
    /// Namespace -> nombre del PRIMER modulo que lo declara, para traducir
    /// un `import a.b.c;` al nombre con que se registra.
    std::unordered_map<std::string, std::string> ns_to_modname;
    /// Namespace -> TODOS los modulos que lo declaran (namespace parcial),
    /// en orden de trabajo.
    std::unordered_map<std::string, ModuleIndices> ns_to_modules;
    /// Lo que el manifiesto auto-importa, y el arbol al que NO se aplica.
    AutoImportNs auto_imports;
    std::string auto_import_owner_dir;

    /**
     * @brief El modulo al que se refiere un import.
     *
     * Por NAMESPACE COMPLETO si el import lo es, y solo si no, por nombre de
     * fichero.  Es la unica forma de resolverlo: cualquier otra reintroduce
     * la confusion entre homonimos.
     *
     * @param req El import.
     * @return Su indice, o @ref kNoModule.
     */
    size_t find(const ImportRequest &req) const;

    /**
     * @brief Un modulo por su nombre de fichero, y nada mas.
     *
     * Para lo que solo trae el nombre: los registros de dependencias de un
     * `.vxi` guardan eso y nada mas, asi que ahi no hay otra forma.  LIMITE
     * conocido: con dos ficheros homonimos devuelve el primero.
     *
     * @param name Nombre del modulo.
     * @return Su indice, o @ref kNoModule.
     */
    size_t find_by_name(const std::string &name) const;

    /**
     * @brief Los modulos que declaran un namespace.
     * @param ns Namespace con puntos.
     * @return Sus indices; nulo si ninguno lo declara.
     */
    const ModuleIndices *modules_of_namespace(const std::string &ns) const;

    /**
     * @brief Los imports de un modulo, con los auto-importados incluidos.
     * @param pm El modulo; sin AST no tiene ninguno que leer.
     * @return Sus imports, en orden de declaracion.
     */
    std::vector<ImportRequest> imports_of(const ProjectModuleWork &pm) const;
};

/**
 * @brief Construye los indices del proyecto.
 * @param work             Los modulos, con su AST.
 * @param auto_imports     Lo que el manifiesto auto-importa.
 * @param auto_owner_dir   El arbol del paquete que lo declara.
 * @return Los indices.
 */
ModuleLookup build_module_lookup(const std::vector<ProjectModuleWork> &work,
                                 AutoImportNs auto_imports,
                                 std::string auto_owner_dir);

/**
 * @brief Los imports que declara un AST, mas los auto-importados.
 *
 * Entra en los namespaces: en la forma `namespace a.b.c;` los imports quedan
 * DENTRO del NamespaceDecl.
 *
 * @param mod           El AST.
 * @param ns_to_modname Traduccion de namespace a nombre de modulo; nulo si
 *                      no se tiene.
 * @param auto_imports  Lo auto-importable; nulo si nada.
 * @param owner_dir     El arbol al que no se le aplica.
 * @param self_path     La ruta del propio modulo.
 * @return Los imports.
 */
std::vector<ImportRequest> collect_imports(
    const ast::ModuleNode &mod,
    const std::unordered_map<std::string, std::string> *ns_to_modname,
    const AutoImportNs *auto_imports, const std::string &owner_dir,
    const std::string &self_path);

/**
 * @brief Huella de lo que un modulo VE de una de sus dependencias.
 *
 * Es lo unico que le puede afectar de ella, y por eso es lo que se guarda en
 * su tabla de dependencias y lo que se recomprueba al reusar lo compilado.
 * Con `only` es la de esos simbolos; sin el (llano, `only *`, re-export), la
 * interfaz entera.
 *
 * @param dep_vxi Interfaz de la dependencia.
 * @param req     Como se importo.
 * @return La huella.
 */
uint64_t used_surface_hash(const VxiModule &dep_vxi, const ImportRequest &req);

/**
 * @brief Las dependencias de un modulo, directas y transitivas.
 *
 * Las directas se resuelven por @ref ModuleLookup::find; las de mas abajo
 * salen de las tablas de dependencias de los `.vxi`, que solo traen el nombre
 * (ver @ref ModuleLookup::find_by_name).
 *
 * @param lookup  Los indices del proyecto.
 * @param work    Los modulos.
 * @param imports Los imports directos del modulo.
 * @return Los indices, en orden de recorrido en anchura: primero los
 *         directos, luego los de sus dependencias.  Sin repetidos.
 */
ModuleIndices transitive_dependencies(const ModuleLookup &lookup,
                                      const std::vector<ProjectModuleWork> &work,
                                      const std::vector<ImportRequest> &imports);

/**
 * @brief El nivel topologico de cada modulo: 0 sin dependencias, y si no uno
 *        mas que la mas alta.
 *
 * Los del mismo nivel no dependen entre si y se pueden compilar a la vez.
 *
 * @param work   Los modulos, ya en orden topologico.
 * @param lookup Los indices.
 * @return Un nivel por modulo.
 */
std::vector<int> compute_module_levels(const std::vector<ProjectModuleWork> &work,
                                       const ModuleLookup &lookup);

} // namespace vx

#endif // VX_PROJECT_MODULE_IMPORTS_H
