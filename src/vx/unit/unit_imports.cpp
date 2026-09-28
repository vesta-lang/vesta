/**
 * @file unit_imports.cpp
 * @brief Crear el comprobador de tipos de un modulo, configurarlo e inyectar
 *        en el todo lo que el modulo importa.
 *
 * Todo esto ocurre ANTES de comprobar tipos: el comprobador tiene que ver los
 * simbolos, los namespaces, las plantillas y los metodos de `impl` de las
 * dependencias desde la primera linea del modulo.
 */
#include "vx/unit/compile_unit.h"

#include "vx/compiler.h"
#include "vx/module/module_interop.h"
#include "vx/module/namespace_names.h" // namespace_symbol_path
#include "vx/module/vxi_format.h"
#include "vx/project/module_imports.h"
#include "vx/project/module_work.h"
#include "vx/type_checker.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace vx {

namespace {

/**
 * @brief Si un simbolo de una interfaz es `internal`.
 * @param s El simbolo.
 * @return @c true si lo es.
 */
bool is_internal_symbol(const VxiSymbol &s) { return s.is_internal; }

/**
 * @brief La interfaz de un dep tal como la ve el consumidor: sin sus simbolos
 *        `internal` si el dep es de OTRO paquete.
 *
 * NS.3: filtrar los simbolos `internal` de un dep que pertenece a OTRO paquete
 * (package_id distinto, ambos no vacios).  Dentro del mismo paquete, internal
 * es visible (no se filtra).  El storage lo aporta el caller (vive lo que dure
 * el uso del &const devuelto).
 *
 * @param consumer_pkgid El PackageId del consumidor.
 * @param v              La interfaz del dep.
 * @param storage        Donde se deja la copia filtrada, si hace falta.
 * @return @p v tal cual, o @p storage ya filtrado.
 */
const VxiModule &without_foreign_internals(const std::string &consumer_pkgid,
                                           const VxiModule &v,
                                           VxiModule &storage) {
    if (consumer_pkgid.empty() || v.package_id.empty() ||
        consumer_pkgid == v.package_id) {
        return v; // mismo paquete (o anonimo) -> internal visible
    }
    bool any_internal = false;
    for (const auto &s : v.symbols)
        if (s.is_internal) {
            any_internal = true;
            break;
        }
    if (!any_internal) return v;
    storage = v;
    auto &syms = storage.symbols;
    syms.erase(std::remove_if(syms.begin(), syms.end(), is_internal_symbol),
               syms.end());
    return storage;
}

/**
 * @brief Crea el comprobador de tipos del modulo y le da lo que no depende de
 *        los imports: el artefacto comptime, el reparto de genericos, lo que
 *        `@Target` dejo fuera y los namespaces propios.
 * @param env               Entorno del proyecto.
 * @param i                 Indice del modulo.
 * @param inline_namespaces Los namespaces propios ya aplanados.
 */
void create_unit_checker(const UnitEnv &env, size_t i,
                         const std::vector<FlattenedNamespace> &inline_namespaces) {
    ProjectModuleWork &pm = (*env.work)[i];
    pm.tc = std::make_unique<TypeChecker>(*pm.ast, pm.diags);
    /* El conjunto comptime ya compilado, en memoria: TODOS los modulos lo
     * ven desde su primer call site.  Es lo que evita que un modulo se
     * compile antes de que exista la maquina que genera parte de su
     * codigo -- y salga con cuerpos de `asm` vacios. */
    pm.tc->set_comptime_artifact(env.opts_modules->comptime_artifact);
    /* El reparto de instanciaciones genericas del proyecto: que
     * `procesa<S0>` la produzca UN modulo y no todos.  Ver
     * @ref vx::GenericInstanceRegistry para lo que costaba no hacerlo. */
    pm.tc->set_generic_instances(env.generic_instances, i);

    pm.tc->register_target_skipped_all(*env.target_skipped);

    /* Si el editor pidio los valores comptime del documento -- el raiz --,
     * las locales de sus bloques `comptime { }` se capturan AL EVALUARLOS:
     * despues ya no existen. */
    if (i + 1 == env.work->size() && env.opts->dump_comptime_values)
        pm.tc->set_capture_comptime_block_locals(true);

    for (const auto &ins : inline_namespaces) {
        const uint32_t ns_idx =
            pm.tc->register_imported_namespace(ins.name, ins.name);
        for (const auto &sym : ins.symbols) {
            TypeChecker::ImportedNamespace::Sym ns_sym;
            ns_sym.kind = (sym.kind == FlattenedNamespace::Sym::Function)
                              ? 0
                              : (sym.kind == FlattenedNamespace::Sym::Type ? 2 : 1);
            ns_sym.mangled_label = sym.mangled_label;
            pm.tc->register_namespace_symbol(ns_idx, sym.public_name,
                                             std::move(ns_sym));
            // NS.2 round-trip: recordar que este mangled_label pertenece al
            // namespace declarado `ins.name` con nombre publico
            // `sym.public_name`, para el export al .vxi.
            pm.tc->register_declared_ns_symbol(sym.mangled_label, ins.name,
                                               sym.public_name);
        }
    }
}

/**
 * @brief Inyecta un import SIN `only`: registra el namespace del dep y sus
 *        plantillas, y si es `public import` lo reexporta.
 * @param env            Entorno del proyecto.
 * @param pm             El modulo consumidor.
 * @param consumer_pkgid Su PackageId.
 * @param req            El import.
 * @param dep_idx        El modulo al que se refiere.
 * @param dep_vxi        La interfaz del dep, ya filtrada.
 * @param dep_alias_srcs Las interfaces de lo que el dep importa.
 */
void inject_plain_import(const UnitEnv &env, ProjectModuleWork &pm,
                         const std::string &consumer_pkgid,
                         const ImportRequest &req, size_t dep_idx,
                         const VxiModule &dep_vxi,
                         const std::vector<const VxiModule *> &dep_alias_srcs) {
    const std::vector<ProjectModuleWork> &work = *env.work;
    // M.7: registrar namespace.
    register_namespace_for_import(*pm.tc, req.local_name, req.module_name,
                                  dep_vxi);
    // #cross-module-generics: inyectar TODAS las plantillas del
    // dep bajo el namespace (`lib.Caja<i64>`).
    inject_generic_templates_from_vxi(*pm.tc, dep_vxi, /*wanted=*/{},
                                      req.local_name,
                                      /*alias_unqualified=*/{}, dep_alias_srcs);
    // Namespace PARCIAL: registrar tambien los simbolos de los
    // OTROS ficheros que declaran el mismo `namespace X;` (p.ej.
    // std.types + std/types/x86_64.vx).  Sin esto, `import
    // std.types` solo veia el primer fichero y los tipos del arch
    // file (`std.types.uintptr`) no resolvian.
    const ModuleIndices *ns_mods =
        req.by_namespace ? env.lookup->modules_of_namespace(req.ns_path)
                         : nullptr;
    if (ns_mods != nullptr) {
        for (const size_t other : *ns_mods) {
            if (other == dep_idx) continue; // ya registrado
            const std::string &other_mn = work[other].module_name.str();
            VxiModule other_store;
            const VxiModule &other_vxi = without_foreign_internals(
                consumer_pkgid, work[other].vxi, other_store);
            register_namespace_for_import(*pm.tc, req.local_name, other_mn,
                                          other_vxi);
            inject_generic_templates_from_vxi(
                *pm.tc, other_vxi, /*wanted=*/{}, req.local_name,
                /*alias_unqualified=*/{}, dep_alias_srcs);
        }
    }
    // M.reexport ext: para `public import "base";` (sin only),
    // inyectar TAMBIEN cada simbolo publico del dep como si
    // fuera un `only A, B, ...` sintetico Y marcarlo
    // re-export.  Sin esto, `mid` con `public import "base"`
    // no reexpone ningun simbolo de base a sus propios
    // consumidores.
    if (req.is_public_reexport) {
        std::vector<TypeChecker::VxiOnlyEntry> synth_only;
        for (const auto &sym : dep_vxi.symbols) {
            // skip simbolos privados o synthetic (mangled).
            if (sym.name.empty()) continue;
            if (sym.name[0] == '_') continue;
            synth_only.push_back({sym.name, ""});
        }
        // Cualificar por NAMESPACE, no por fichero: en un namespace
        // parcial todos sus ficheros deben dar la MISMA identidad.
        const std::string qual = (req.by_namespace && !req.ns_path.empty())
                                     ? namespace_symbol_path(req.ns_path)
                                     : req.module_name;
        auto missing = import_vxi_into_typechecker_with_missing(
            *pm.tc, dep_vxi, synth_only, qual, req.loc);
        (void)missing; // best-effort; los privados ya fueron
                       //              filtrados al construir el
                       //              .vxi.
        for (const auto &os : synth_only) {
            pm.tc->mark_imported(os.name, /*is_reexport=*/true);
        }
        // Re-exportar TAMBIEN las plantillas genericas / comptime
        // fns / @Macros: viven en `generic_templates` (texto
        // fuente), NO en `symbols`.  Sin esto, `public import
        // std.comptime. basics` no reexpone `source`/`inject`
        // (comptime fns) -> `import std.comptime only source` daba
        // "no exporta source". La rama is_plain de arriba YA las
        // inyecto en este modulo (inject_generic_templates_from_vxi
        // con ns_prefix=local); aqui solo las anyadimos a sus
        // exports marcadas re-export para que floten a su .vxi.  NO
        // re-inyectar (doble inject -> "redefinicion de comptime
        // fn").
        if (!dep_vxi.generic_templates.empty()) {
            for (const auto &g : dep_vxi.generic_templates) {
                if (g.name.empty() || g.name[0] == '_') continue;
                ast::GenericTemplateExport tex;
                tex.name = g.name;
                tex.kind = g.kind;
                tex.source = g.source;
                tex.is_public = true;
                pm.tc->add_reexported_generic_template(std::move(tex));
                pm.tc->mark_imported(g.name, /*is_reexport=*/true);
            }
        }
    }
}

/**
 * @brief Inyecta un import CON `only`: sus plantillas y los simbolos
 *        listados, reintentando los que falten en los otros ficheros del
 *        namespace.
 * @param env            Entorno del proyecto.
 * @param pm             El modulo consumidor.
 * @param consumer_pkgid Su PackageId.
 * @param req            El import.
 * @param dep_idx        El modulo al que se refiere.
 * @param dep_vxi        La interfaz del dep, ya filtrada.
 * @param dep_alias_srcs Las interfaces de lo que el dep importa.
 * @return @c false si falta algun simbolo pedido.
 */
bool inject_only_import(const UnitEnv &env, ProjectModuleWork &pm,
                        const std::string &consumer_pkgid,
                        const ImportRequest &req, size_t dep_idx,
                        const VxiModule &dep_vxi,
                        const std::vector<const VxiModule *> &dep_alias_srcs) {
    const std::vector<ProjectModuleWork> &work = *env.work;
    // #cross-module-generics: inyectar las plantillas del dep
    // (sin namespace -> nombre directo `Caja<i64>`).  Se inyectan
    // TODAS (no solo las listadas en `only`) para que las
    // dependencias entre plantillas se satisfagan (p.ej. una fn
    // generica con bound de un `concept` del mismo modulo).  Son
    // inertes si no se usan (se monomorphizan solo on-use).  Se
    // hace ANTES de la inyeccion de simbolos para que el template
    // exista en mod_.decls cuando run() registre los templates.
    // Los nombres listados en `only` que sean comptime/macro fns
    // deben quedar invocables SIN cualificar (como una fn regular
    // via only) -> pasarlos como alias_unqualified.
    // Nota: el matching en el inject es por el nombre ORIGINAL del
    // decl (`nm`), por eso usamos os.name.  El `as <rename>` de un
    // macro invocado sin cualificar es un caso raro no cubierto
    // aun.
    std::unordered_set<std::string> only_alias;
    for (const auto &os : req.only_symbols)
        only_alias.insert(os.name);
    inject_generic_templates_from_vxi(*pm.tc, dep_vxi,
                                      /*wanted=*/{},
                                      /*ns_prefix=*/"", only_alias,
                                      dep_alias_srcs);
    // M2.d: inyeccion directa via only.  M6.a.3: usar la variante
    // que devuelve los missing para emitir diagnostico claro.
    /* Cualificar por NAMESPACE, no por fichero.  Hacerlo con el
     * nombre de FICHERO era el origen de que un mismo tipo tuviera
     * varias identidades: `std.types` lo declaran `types.vx`,
     * `types/arm64.vx` y `types/x86_64.vx`, y el resolver devuelve
     * el PRIMERO que encuentra el escaneo del disco -- el mismo
     * `uintptr` entraba como `arm64__uintptr` o como
     * `std__types__uintptr` y no unificaba consigo mismo --.  El
     * namespace es el mismo para todos sus ficheros. */
    const std::string qual = (req.by_namespace && !req.ns_path.empty())
                                 ? namespace_symbol_path(req.ns_path)
                                 : req.module_name;
    auto missing = import_vxi_into_typechecker_with_missing(
        *pm.tc, dep_vxi, req.only_symbols, qual, req.loc);
    // Namespace PARCIAL: un `import std.types only uintptr`
    // resuelve `req.module_name` al PRIMER fichero del namespace
    // (p.ej. arm64), donde el simbolo puede estar @Target-inactivo
    // -> queda en `missing`.  Reintentar los que faltan contra los
    // OTROS ficheros del mismo `namespace X;` (p.ej.
    // std/types/x86_64.vx), igual que el plain-import de arriba.
    // Sin esto, `only X` de un namespace multi-fichero fallaba con
    // "no exporta 'X'".
    const ModuleIndices *ns_mods =
        (!missing.empty() && req.by_namespace)
            ? env.lookup->modules_of_namespace(req.ns_path)
            : nullptr;
    if (ns_mods != nullptr) {
        // `retry` = los only_symbols que aun faltan.
        std::vector<TypeChecker::VxiOnlyEntry> retry;
        for (const auto &os : req.only_symbols) {
            for (const auto &m : missing) {
                if (os.name == m) {
                    retry.push_back(os);
                    break;
                }
            }
        }
        for (const size_t other : *ns_mods) {
            if (retry.empty()) break;
            if (other == dep_idx) continue; // ya se probo
            VxiModule other_store;
            const VxiModule &other_vxi = without_foreign_internals(
                consumer_pkgid, work[other].vxi, other_store);
            // Mismo cualificador que arriba: los ficheros
            // restantes del namespace parcial NO pueden dar una
            // identidad distinta a la del primero.
            auto still = import_vxi_into_typechecker_with_missing(
                *pm.tc, other_vxi, retry, qual, req.loc);
            // reducir retry a los que aun faltan tras este
            // fichero
            std::vector<TypeChecker::VxiOnlyEntry> next_retry;
            for (const auto &os : retry) {
                for (const auto &m : still) {
                    if (os.name == m) {
                        next_retry.push_back(os);
                        break;
                    }
                }
            }
            retry = std::move(next_retry);
        }
        // el `missing` final = lo que aun no aparecio en NINGUN
        // fichero del namespace.
        std::vector<std::string> final_missing;
        for (const auto &os : retry)
            final_missing.push_back(os.name);
        missing = std::move(final_missing);
    }
    // #cross-module-generics: un nombre `only` puede ser una
    // PLANTILLA generica (no esta en symbols sino en
    // generic_templates) -> no es "missing".
    std::unordered_set<std::string> gen_names;
    for (const auto &g : dep_vxi.generic_templates)
        gen_names.insert(g.name);
    for (const auto &m : missing) {
        if (gen_names.count(m)) continue; // es un template: OK
        pm.diags.diag(req.loc, DiagLevel::ERR, "VX4010", {req.module_name, m});
    }
    // Recalcular missing real (excluyendo templates) para el
    // early-abort de abajo.
    {
        std::vector<std::string> real_missing;
        for (const auto &m : missing)
            if (!gen_names.count(m)) real_missing.push_back(m);
        missing = std::move(real_missing);
    }
    if (!missing.empty()) return false;
    //  M.L23: marcar cada simbolo importado como
    // (imported, is_reexport).  El export del .vxi del
    // modulo actual filtra los importados NO marcados como
    // public.
    for (const auto &os : req.only_symbols) {
        const std::string &local = os.rename.empty() ? os.name : os.rename;
        pm.tc->mark_imported(local, req.is_public_reexport);
    }
    return true;
}

} // namespace

bool inject_unit_imports(const UnitEnv &env, size_t i,
                         const std::vector<FlattenedNamespace> &inline_namespaces,
                         std::vector<ImportRequest> &imports) {
    const std::vector<ProjectModuleWork> &work = *env.work;
    const ModuleLookup &lookup = *env.lookup;
    ProjectModuleWork &pm = (*env.work)[i];

    create_unit_checker(env, i, inline_namespaces);

    // ANTES de typecheck: inyectar simbolos de los deps via .vxi.
    // Dos modos:
    //   - `import "x" only A, B;`   -> inyecta A, B directos en scope.
    //   - `import "x" [as alias];`  -> registra namespace para `x.A` o
    //                                   `alias.A` ( M.7).
    imports = lookup.imports_of(pm);

    // LANG.fix-3: pre-importar las .vxi de los deps TRANSITIVOS
    // antes de procesar los imports explicitos.  Si main tiene
    // `import "outer";` + outer depende de inner, main necesita
    // inner.vxi en su TC ANTES de procesar outer (porque outer.vxi
    // referencia tipos como `inner__Bar`).  Sin esto el resolver de
    // fields falla con "void" para tipos del dep transitivo.
    //
    // Estrategia: recorrer la cadena topo de los deps directos y
    // pre-importar cada dep que NO este ya en los imports explicitos.
    // Solo registramos namespaces (no inyectamos al scope) para no
    // contaminar el namespace global del consumer.
    // Recolectar deps TRANSITIVOS (no incluidos en imports explicitos)
    // y pre-importarlos como namespaces silenciosos ANTES del loop de
    // imports explicitos.  Asi cuando outer.vxi se procesa, las
    // referencias a tipos de inner ya estan en class_layouts_ via la
    // pre-importacion de inner.  Cada namespace solo se registra una
    // vez (los explicitos van por el loop normal mas abajo).
    // Recolectar TODOS los deps (directos + transitivos) y pre-importar
    // en orden topo inverso (mas profundo primero).  El register_*
    // usa operator[] (overwrite) y register_imported_namespace ahora
    // dedupea por local_name, asi que registrar el mismo dep dos
    // veces es no-op.

    // NS.3: PackageId del consumidor, para filtrar los simbolos `internal`
    // de un dep que pertenece a OTRO paquete (ver without_foreign_internals).
    const std::string consumer_pkgid = unit_package_id(env, i);
    // Si una dependencia no compilo, este modulo tampoco vale.  Sin esta
    // comprobacion se compilaba igual -- con la superficie del dep VACIA --
    // y, lo que es peor, se PERSISTIA su interfaz: la siguiente compilacion
    // hacia cache hit sobre esa interfaz degradada y el fallo sobrevivia al
    // arreglo del dep.  Solo se nota si el import no lleva `only`, porque
    // entonces no hay ningun simbolo concreto que echar en falta.
    for (const auto &req : imports) {
        const size_t dep_idx = lookup.find(req);
        if (dep_idx >= work.size() || work[dep_idx].ok) continue;
        pm.diags.diag(req.loc, DiagLevel::ERR, "VX4008",
                      {req.ns_path.empty() ? req.module_name : req.ns_path});
        return false;
    }
    /* Las dependencias directas y transitivas, UNA vez: las usan el
     * pre-registro de namespaces de aqui abajo y la inyeccion de los
     * metodos de `impl` de mas abajo. */
    const ModuleIndices all_deps = transitive_dependencies(lookup, work, imports);
    // Del mas profundo al mas cercano: un tipo de la dependencia de una
    // dependencia tiene que existir antes que quien lo nombra.
    for (size_t k = all_deps.size(); k-- > 0;) {
        const ProjectModuleWork &transit = work[all_deps[k]];
        const std::string &mn = transit.module_name.str();
        VxiModule tstore;
        register_namespace_for_import(
            *pm.tc, mn, mn,
            without_foreign_internals(consumer_pkgid, transit.vxi, tstore));
    }

    /* Un import que no resuelve NO corta el bucle: se dicen TODOS los que
     * faltan de una vez.  Pero el modulo ya no vale, y seguir hasta el
     * comprobador solo anadiria errores en cascada sobre tipos que salen
     * `void` porque su modulo no esta. */
    bool all_found = true;
    for (auto &req : imports) {
        const size_t dep_idx = lookup.find(req);
        if (dep_idx >= work.size()) {
            // Un import que no resuelve se saltaba en silencio: no se
            // inyectaba ninguno de sus simbolos y la compilacion seguia
            // como si nada, fallando mucho mas tarde y en otro sitio -- o
            // peor, dando un resultado equivocado.  Quien escribio el
            // import merece enterarse aqui.
            SourceLoc iloc;
            iloc.set_file(pm.canonical_path);
            /* Al saco del MODULO, que es el que el bucle de mas abajo
             * vuelca en el del proyecto: esto corre en varios hilos y el
             * saco del proyecto es uno solo. */
            pm.diags.diag(std::move(iloc), DiagLevel::ERR, "VX4009",
                          {req.module_name});
            all_found = false;
            continue;
        }
        const ProjectModuleWork &dep = work[dep_idx];
        VxiModule dep_filtered_storage;
        const VxiModule &dep_vxi =
            without_foreign_internals(consumer_pkgid, dep.vxi,
                                      dep_filtered_storage);
        // Los modulos de los que el dep importa.  Sus plantillas viajan
        // como texto y se re-parsean aqui, asi que sus firmas pueden
        // nombrar tipos que el dep trajo de un tercero (`usize`, de
        // `std.types`); sin esto ese nombre no existe al re-parsear y el
        // parametro se queda en `void`.
        std::vector<const VxiModule *> dep_alias_srcs;
        for (const auto &dr : dep_vxi.deps) {
            const size_t src = lookup.find(dr);
            if (src < work.size()) dep_alias_srcs.push_back(&work[src].vxi);
        }
        // `only *` (glob): expandir a TODOS los simbolos publicos del dep,
        // como si el usuario hubiera listado cada uno (nombre directo, sin
        // rename).  Se hace aqui -- no en el mapeo AST -- porque necesita
        // el .vxi del dep (la lista de sus simbolos).  Con `public import`
        // los re-exporta (req.is_public_reexport se propaga a
        // mark_imported).
        if (req.only_all && req.only_symbols.empty()) {
            // Inyectar TODOS los simbolos publicos del dep.  El .vxi ya
            // filtro los sinteticos del compilador, asi que NO descartamos
            // por prefijo `_`: un `public __NR_write` (convencion POSIX) es
            // legitimo y debe entrar al scope.
            for (const auto &sym : dep_vxi.symbols) {
                if (sym.name.empty()) continue;
                req.only_symbols.push_back({sym.name, ""});
            }
        }
        if (req.is_plain) {
            inject_plain_import(env, pm, consumer_pkgid, req, dep_idx, dep_vxi,
                                dep_alias_srcs);
        } else if (!inject_only_import(env, pm, consumer_pkgid, req, dep_idx,
                                       dep_vxi, dep_alias_srcs)) {
            return false;
        }
    }
    /* Antes aqui se marcaba `pm.ok = false` y se seguia, y el final de la
     * compilacion del modulo lo volvia a poner a `true`: el error quedaba en
     * los diagnosticos pero la bandera decia que el modulo estaba bien. */
    if (!all_found) return false;

    // NS.6-ext: re-apendear los metodos de `extension`/`impl` que
    // declararon los deps (directos + transitivos) al layout del tipo
    // destino en este consumidor.  Asi `obj.metodo()` resuelve cross-modulo
    // (dispatch estatico al mangled_label del .velb del dep).  Los layouts
    // de los tipos importados ya estan registrados (import loop de arriba).
    for (const size_t dep : all_deps) {
        for (const auto &em : work[dep].vxi.ext_methods) {
            pm.tc->inject_imported_ext_method(
                em.target_key, em.target_is_class, em.name, em.return_type,
                em.param_types, em.mangled_label, em.attrs.visibility);
        }
    }
    return true;
}

} // namespace vx
