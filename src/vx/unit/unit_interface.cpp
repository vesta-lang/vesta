/**
 * @file unit_interface.cpp
 * @brief Construir la interfaz (`.vxi`) de un modulo ya compilado: lo que
 *        exporta, su conjunto comptime, su paquete, su objetivo, de que
 *        depende y lo que sabe de si mismo.
 */
#include "vx/unit/compile_unit.h"

#include "vx/contracts_collect.h"
#include "vx/module/module_interop.h" // export_typechecker_to_vxi
#include "vx/module/vxi_format.h"
#include "vx/parser.h" // get_aot_condcomp_target
#include "vx/project/module_imports.h"
#include "vx/project/module_names.h" // module_symbol_prefix
#include "vx/project/module_work.h"

#include <string>
#include <utility>

namespace vx {

namespace {

/**
 * @brief Si @p decls declara alguna clase.
 *
 * AL NIVEL DE ARRIBA Y NO RECURSIVA, que es exactamente lo que miraba el
 * tree-shake cuando recorria el AST a mano.  Se conserva el criterio a
 * proposito: cambiarlo aqui cambiaria QUE modulos se eliminan, y eso es una
 * decision aparte de dejar de sostener un AST para contestarlo.
 *
 * @param decls Las declaraciones de nivel superior del modulo.
 * @return @c true si alguna es una clase.
 */
bool declares_classes(const std::vector<std::unique_ptr<ast::Node>> &decls) {
    for (const auto &d : decls)
        if (d && d->kind == ast::NodeKind::ClassDecl) return true;
    return false;
}

} // namespace

std::string unit_package_id(const UnitEnv &env, size_t i) {
    const std::vector<std::string> &module_pkgid_override =
        *env.module_package_override;
    return (i < module_pkgid_override.size() &&
            !module_pkgid_override[i].empty())
               ? module_pkgid_override[i]
               : *env.project_package_id;
}

void build_unit_interface(const UnitEnv &env, size_t i,
                          const UnitCacheKeys &keys,
                          const std::vector<ImportRequest> &imports) {
    const std::vector<ProjectModuleWork> &work = *env.work;
    const ModuleLookup &lookup = *env.lookup;
    ProjectModuleWork &pm = (*env.work)[i];
    const bool is_root = (i + 1 == work.size());

    //  M.5: export con strip_prefix = `<module>__` para que el
    // .vxi exponga nombres publicos sin el mangle.  El consumidor
    // importa "sumar" pero la FunctionSig lleva mangled_label="lib__sumar".
    const std::string strip_prefix =
        is_root ? std::string() // root: sin prefix (no se exporta)
                : module_symbol_prefix(pm.module_name.str());
    export_typechecker_to_vxi(*pm.tc, keys.source_hash, pm.vxi, strip_prefix);
    /* v18: y el conjunto comptime de ESTE modulo, para que el `.vxi` lo
     * lleve.  Extraerlo necesita el AST, y la proxima compilacion que sirva
     * este modulo del cache no lo va a parsear: si no viaja aqui, se
     * pierde. */
    pm.vxi.comptime_unit_source = pm.comptime_unit_source;
    pm.vxi.comptime_unit_hash = pm.comptime_unit_hash;
    pm.vxi.comptime_unit_names = pm.comptime_unit_names;
    pm.vxi.comptime_unit_not_collected = pm.comptime_unit_not_collected;

    //  NS.3: estampar el PackageId en el .vxi del modulo.  Por defecto
    // el del proyecto (vx.toml); si el modulo declaro `namespace X
    // @id(..)`, ese override gana (identidad ABI por-namespace).  El
    // override se capturo antes del flatten (que borra el NamespaceDecl del
    // AST).
    pm.vxi.package_id = unit_package_id(env, i);

    // v13: atar el .vxi al OBJETIVO, pero solo si el modulo usa @Target --
    // lo que declara depende entonces del target y su artefacto no vale
    // para otro.  Los demas (la inmensa mayoria) siguen con un unico .vxi
    // compartido, de modo que cambiar de objetivo no recompila la stdlib
    // entera.
    //
    // Sin esto, un .vxi generado compilando para arm64 se seguia sirviendo
    // en un build x86-64 y metia sus tipos en la resolucion: el mismo
    // `uintptr` acababa con dos identidades segun la ruta de importacion.
    if (pm.ast && pm.ast->uses_conditional_target) {
        // El mismo que se compara al leerlo: ver `vxi_active_target`.
        pm.vxi.target = vxi_active_target();
    }

    //  M4.ext L.13: poblar dep table con los (name, abi_hash) de
    // los deps directos del modulo.  El loader del cache verifica
    // estos hashes al cache hit para invalidacion transitiva: si
    // cualquier dep cambio su .vxi (distinto abi_hash), este modulo
    // tambien debe recompilarse.
    pm.vxi.deps.clear();
    for (const auto &req : imports) {
        const size_t dep_idx = lookup.find(req);
        if (dep_idx >= work.size()) continue;
        const ProjectModuleWork &dep = work[dep_idx];
        VxiModule::DepRecord drec;
        /* El nombre del modulo RESUELTO, no el que traia el import: con
         * dos ficheros homonimos pueden no coincidir, y al validar se
         * busca por este. */
        drec.name = dep.module_name.str();
        // Y su namespace, que es lo que lo distingue de un homonimo.
        drec.ns = lookup.namespace_of(dep_idx);
        /* Lo que este modulo VE del dep, no la interfaz entera del dep:
         * anadirle algo publico que nadie usa no tiene por que invalidar a
         * quien no lo usa.  El mismo criterio se aplica al validar. */
        drec.abi_hash = used_surface_hash(dep.vxi, req);
        pm.vxi.deps.push_back(std::move(drec));
    }

    /* LO QUE ESTE MODULO SABE DE SI MISMO, apuntado aqui porque aqui es
     * donde su AST existe -- y apuntarlo es lo que permite que deje de
     * existir.  Los tres consumidores estan a dos mil lineas de aqui y
     * ninguno quiere un AST: quieren estas respuestas.  Ver
     * `release_compiled_module`.
     *
     * Y AQUI, ANTES DE SERIALIZAR LA INTERFAZ, no despues.  Estaba al final
     * de la compilacion del modulo, cientos de lineas por debajo de
     * `vxi_emit`, asi que `declares_classes` se ponia en la estructura
     * cuando el fichero ya estaba escrito: el `.vxi` de disco decia SIEMPRE
     * que no.  Quien servia el modulo de la cache no lo parseaba -- ese es
     * el motivo de guardarlo --, leia el falso y el tree-shake se quedaba
     * sin la unica razon por la que no debe eliminar esa dependencia. */
    if (pm.ast) {
        pm.has_classes = declares_classes(pm.ast->decls);
        /* Y AL ARTEFACTO, que es lo que lo hace util: quien sirva este
         * modulo del cache no lo parseara, asi que esta es la unica
         * ocasion de averiguarlo.  Ver `VxiHeader::module_flags`. */
        pm.vxi.declares_classes = pm.has_classes;
        collect_function_contracts(pm.ast->decls, *pm.tc, pm.contracts);
    }
}

} // namespace vx
