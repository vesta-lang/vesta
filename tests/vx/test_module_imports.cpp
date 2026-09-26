/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/vx/test_module_imports.cpp
 * @brief Comprueba como se resuelve un import en un proyecto.
 *
 * El caso que lo motiva son dos ficheros HOMONIMOS en carpetas distintas --
 * `std/os/linux.vx` y `std/syscall/linux.vx`, los dos `linux` --.  Por nombre
 * solo se encuentra uno, y quien buscaba asi se llevaba el que no era: la
 * clave de la cache y la validacion de dependencias se calculaban contra otro
 * modulo.  Aqui se comprueba que un import por namespace encuentra SU modulo
 * aunque su nombre de fichero choque con otro, que el recorrido transitivo no
 * repite ni se pierde, y que un namespace repartido en varios ficheros lista
 * todos por indice.
 */
#include "vx/project/module_imports.h"
#include "vx/project/module_work.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

int g_failures = 0; ///< Cuantas comprobaciones han fallado.

/**
 * @brief Comprueba una condicion y deja constancia si no se cumple.
 * @param cond Lo que tiene que ser cierto.
 * @param what Que se estaba comprobando.
 */
void check(bool cond, const char *what) {
    if (!cond) {
        std::printf("FALLO: %s\n", what);
        ++g_failures;
    }
}

/**
 * @brief Un modulo con nombre y, si se da, un namespace declarado.
 * @param name Nombre del modulo (el de su fichero).
 * @param ns   Namespace que declara; vacio si ninguno.
 * @return El modulo, con un AST minimo.
 */
vx::ProjectModuleWork make_module(const char *name, const char *ns) {
    vx::ProjectModuleWork pm;
    pm.module_name = util::InternedName::intern(name);
    pm.canonical_path = util::InternedName::intern(std::string("/p/") + name);
    pm.ast = std::make_unique<vx::ast::ModuleNode>();
    if (ns[0] != 0) {
        auto nd = std::make_unique<vx::ast::NamespaceDecl>();
        nd->name = ns;
        pm.ast->decls.push_back(std::move(nd));
    }
    return pm;
}

/**
 * @brief Anade a un modulo un registro de dependencia de su interfaz.
 * @param pm   El modulo.
 * @param name Nombre de la dependencia, como lo guarda el `.vxi`.
 */
void add_dep_record(vx::ProjectModuleWork &pm, const char *name) {
    vx::VxiModule::DepRecord r;
    r.name = name;
    pm.vxi.deps.push_back(r);
}

} // namespace

/**
 * @brief Punto de entrada del test.
 * @return 0 si todo paso, 1 si hubo algun fallo.
 */
int main() {
    std::vector<vx::ProjectModuleWork> work;
    work.push_back(make_module("linux", "std.os.linux"));        // 0
    work.push_back(make_module("linux", "std.syscall.linux"));   // 1
    work.push_back(make_module("types", "std.types"));           // 2
    work.push_back(make_module("x86_64", "std.types"));          // 3
    work.push_back(make_module("app", ""));                      // 4

    const vx::ModuleLookup lk =
        vx::build_module_lookup(work, vx::AutoImportNs{}, std::string());

    // Por namespace: cada homonimo el suyo.
    vx::ImportRequest by_ns;
    by_ns.by_namespace = true;
    by_ns.ns_path = "std.syscall.linux";
    by_ns.module_name = "linux";
    check(lk.find(by_ns) == 1, "import por namespace encuentra SU homonimo");
    by_ns.ns_path = "std.os.linux";
    check(lk.find(by_ns) == 0, "y el otro homonimo el suyo");

    // Por ruta: solo hay nombre, y gana el primero (limite conocido).
    vx::ImportRequest by_path;
    by_path.module_name = "linux";
    check(lk.find(by_path) == 0, "por nombre gana el primero");
    check(lk.find_by_name("app") == 4, "busqueda por nombre");
    by_path.module_name = "no_existe";
    check(lk.find(by_path) == vx::kNoModule, "lo que no existe se dice");

    // Un namespace en dos ficheros: los dos, por indice y en orden.
    const vx::ModuleIndices *types = lk.modules_of_namespace("std.types");
    check(types != nullptr && types->size() == 2 && (*types)[0] == 2 &&
              (*types)[1] == 3,
          "namespace parcial lista sus dos modulos");
    check(lk.modules_of_namespace("nada") == nullptr,
          "namespace desconocido: ninguno");
    check(lk.ns_to_modname.at("std.types") == "types",
          "el namespace se traduce al primer modulo que lo declara");

    // Un registro de dependencia de un `.vxi`: por namespace el suyo, y sin
    // namespace, por nombre.
    vx::VxiModule::DepRecord rec;
    rec.name = "linux";
    rec.ns = "std.syscall.linux";
    check(lk.find(rec) == 1, "registro homonimo: gana su namespace");
    rec.ns.clear();
    check(lk.find(rec) == 0, "registro sin namespace: por nombre");
    check(lk.namespace_of(1) == "std.syscall.linux",
          "el namespace que identifica a un modulo");
    check(lk.namespace_of(4).empty(), "modulo sin namespace");

    // Transitivo: app -> syscall.linux -> types -> x86_64 (y types otra vez).
    add_dep_record(work[1], "types");
    add_dep_record(work[2], "x86_64");
    add_dep_record(work[3], "types"); // ciclo por nombre: no debe repetirse
    std::vector<vx::ImportRequest> imports;
    by_ns.ns_path = "std.syscall.linux";
    imports.push_back(by_ns);
    imports.push_back(by_ns); // el mismo import dos veces
    const vx::ModuleIndices deps =
        vx::transitive_dependencies(lk, work, imports);
    check(deps.size() == 3, "tres dependencias, sin repetidas");
    check(deps.size() == 3 && deps[0] == 1 && deps[1] == 2 && deps[2] == 3,
          "en anchura, empezando por la directa");

    if (g_failures == 0) std::printf("test_module_imports: OK\n");
    return g_failures == 0 ? 0 : 1;
}
