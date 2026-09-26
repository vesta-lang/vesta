/**
 * @file unit_cache.cpp
 * @brief Las claves de cache de un modulo, servirlo de la cache y guardarlo en
 *        ella.
 *
 * Tres momentos de la misma cache juntos a proposito: la clave con que se
 * busca, la busqueda y la escritura.  Si la clave con que se guarda y la clave
 * con que se busca se calcularan en sitios distintos, acabarian divergiendo y
 * la cache serviria artefactos de otra compilacion.
 */
#include "vx/unit/compile_unit.h"

#include "ir/ir_emitter.h"
#include "ir/ir_optimizer.h" // ir::opt_level_from_int
#include "ir/ssa_ir_serialize.h"
#include "util/env_flags.h"
#include "util/file_read.h"
#include "util/fs_utils.h" // fs::write_file_atomic
#include "vx/compiler.h"
#include "vx/incremental.h" // CasStore
#include "vx/module/vxi_format.h"
#include "vx/parser.h" // get_aot_condcomp_target
#include "vx/project/module_artifact.h"
#include "vx/project/module_cache_key.h"
#include "vx/project/module_imports.h"
#include "vx/project/module_paths.h"
#include "vx/project/module_work.h"

#include <algorithm>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace vx {

namespace {

/**
 * @brief Si todas las dependencias que el `.vxi` guardado apunto siguen
 *        teniendo la huella con que se guardo.
 * @param env Entorno del proyecto.
 * @param pm  El modulo.
 * @param deps Las dependencias apuntadas en su `.vxi` guardado.
 * @return @c true si ninguna cambio.
 */
bool cached_deps_match(const UnitEnv &env, const ProjectModuleWork &pm,
                       const std::vector<VxiModule::DepRecord> &deps) {
    const std::vector<ProjectModuleWork> &work = *env.work;
    const ModuleLookup &lookup = *env.lookup;
    //  M4.ext L.13: cache transitivo.  El source_hash
    // del modulo coincide, pero alguno de sus deps directos
    // podria haber cambiado.  Verificar que cada DepRecord
    // del .vxi cacheado matchea el abi_hash actual del dep
    // (que ya viene populated en work[] por topo order).
    /* Como importa este modulo cada dep, para recomputar la
     * huella con EL MISMO criterio con que se guardo. */
    /* Por el MODULO resuelto, no por su nombre: el registro
     * y el import se encuentran por el mismo indice. */
    std::unordered_map<size_t, const ImportRequest *> import_by_dep;
    const std::vector<ImportRequest> imports_for_check = lookup.imports_of(pm);
    for (const auto &r : imports_for_check) {
        const size_t idx = lookup.find(r);
        if (idx < work.size()) import_by_dep.emplace(idx, &r);
    }
    bool deps_match = true;
    for (const auto &dep_rec : deps) {
        // Por su namespace, que distingue a los homonimos.
        const size_t dep_idx = lookup.find(dep_rec);
        const auto itc = import_by_dep.find(dep_idx);
        if (dep_idx >= work.size()) {
            // El dep ya no existe -> miss.
            deps_match = false;
            if (env.cache.verbose) {
                std::ostringstream tmp;
                tmp << "[vx-cache] miss (transitivo): dep '" << dep_rec.name
                    << "' no encontrado\n";
                std::lock_guard<std::mutex> lk(*env.verbose_mtx);
                std::cerr << tmp.str();
            }
            break;
        }
        const uint64_t actual =
            itc != import_by_dep.end()
                ? used_surface_hash(work[dep_idx].vxi, *itc->second)
                : work[dep_idx].vxi.abi_hash;
        if (actual != dep_rec.abi_hash) {
            deps_match = false;
            if (env.cache.verbose) {
                std::ostringstream tmp;
                tmp << "[vx-cache] miss (transitivo): dep '" << dep_rec.name
                    << "' cambio (abi_hash old=0x" << std::hex
                    << dep_rec.abi_hash << " new=0x" << actual << std::dec
                    << ")\n";
                std::lock_guard<std::mutex> lk(*env.verbose_mtx);
                std::cerr << tmp.str();
            }
            break;
        }
    }
    return deps_match;
}

/**
 * @brief Intenta servir el modulo de la cache comun por contenido.
 * @param env  Entorno del proyecto.
 * @param pm   El modulo.
 * @param keys Sus claves.
 * @return @c true si quedo servido.
 */
bool serve_from_cas(const UnitEnv &env, ProjectModuleWork &pm,
                    const UnitCacheKeys &keys) {
    if (!keys.cas_key_ok) return false;
    std::vector<uint8_t> blob;
    if (!env.cache.cas->get(keys.cas_key, blob)) return false;
    std::vector<uint8_t> vb, ib;
    if (!unpack_module_artifact(blob, vb, ib)) return false;
    auto pr = vxi_parse(vb.data(), vb.size());
    ir::IrModule dep_mod;
    if (pr.ok && ir::parse_ir_module_cache(ib.data(), ib.size(), dep_mod)) {
        /* Lo mismo que el acierto por ruta, por la misma
         * funcion: servido por una u otra cache, el modulo
         * tiene que quedar igual.  (`vxi_parse` ya lee
         * `abi_hash` de la cabecera; aqui se releia.) */
        adopt_cached_artifact(pm, std::move(pr.module_), std::move(dep_mod));
        if (env.cache.verbose) {
            std::ostringstream tmp;
            tmp << "[vx-cas] hit: " << pm.canonical_path.str() << "\n";
            std::lock_guard<std::mutex> lk(*env.verbose_mtx);
            std::cerr << tmp.str();
        }
        return true; // artefacto reusado del CAS global.
    }
    return false;
}

/**
 * @brief Intenta servir el modulo de la cache junto a su fuente.
 * @param env  Entorno del proyecto.
 * @param pm   El modulo.
 * @param keys Sus claves.
 * @return @c true si quedo servido.
 */
bool serve_from_path_cache(const UnitEnv &env, ProjectModuleWork &pm,
                           const UnitCacheKeys &keys) {
    const uint64_t source_hash = keys.source_hash;
    const ModuleCachePaths &paths =
        module_cache_paths(pm.canonical_path, keys.target_suffix);
    const std::string &vp = paths.vxi;
    const std::string &ip = paths.vxir;
    std::vector<uint8_t> vbytes;
    if (util::read_whole_file(vp, vbytes)) {
        auto pr = vxi_parse(vbytes.data(), vbytes.size());
        // v13: un artefacto atado a OTRO objetivo no sirve.  Solo los
        // modulos que usan @Target llevan objetivo (el resto va con el
        // campo vacio y vale para todos), asi que esto no invalida nada
        // que no dependa de verdad del target.
        //
        // Sin esta comprobacion, un .vxi generado compilando para arm64
        // se servia tal cual en un build x86-64 y metia sus tipos en la
        // resolucion: el mismo `uintptr` acababa con dos identidades
        // (`arm64__uintptr` y `std__types__uintptr`) segun la ruta de
        // importacion, con un error de tipos incomprensible.
        if (pr.ok && !pr.module_.target.empty()) {
            const std::string actual = vxi_active_target();
            if (pr.module_.target != actual) {
                if (env.cache.verbose) {
                    std::ostringstream tmp;
                    tmp << "[vx-cache] miss (objetivo): '"
                        << pm.module_name.str() << "' se genero para "
                        << pr.module_.target << " y se compila para " << actual
                        << "\n";
                    std::cerr << tmp.str();
                }
                pr.ok = false; // fuerza recompilar con este objetivo
            }
        }
        if (pr.ok && pr.module_.source_hash == source_hash &&
            cached_deps_match(env, pm, pr.module_.deps)) {
            // Hash match -> intentar cargar tambien el .vxir.
            std::vector<uint8_t> ibytes;
            if (util::read_whole_file(ip, ibytes) && !ibytes.empty()) {
                // BugFix M.vxir-sd: cargar el modulo COMPLETO
                // (functions + static_data + globals).  El formato
                // viejo (solo functions) perdia el static_data del
                // dep -> relocaciones `code.s_*` colgantes en el
                // `.velb` con cache caliente.  Un `.vxir` viejo
                // falla el magic y cae a recompilar.
                ir::IrModule dep_mod;
                // La interfaz y el IR son dos ficheros que tienen
                // que corresponderse.  Entre validar la primera y
                // leer el segundo, otra compilacion simultanea del
                // mismo modulo puede publicar una version nueva de
                // ambos: se acabaria mezclando la interfaz que se
                // valido con un IR que no es el suyo.  Releerla
                // despues y comprobar que sigue siendo la misma lo
                // descarta; ante la duda, se recompila.
                bool pair_consistent = true;
                {
                    std::vector<uint8_t> vb2;
                    if (!util::read_whole_file(vp, vb2)) {
                        pair_consistent = false;
                    } else {
                        auto pr2 = vxi_parse(vb2.data(), vb2.size());
                        if (!pr2.ok || pr2.module_.source_hash != source_hash)
                            pair_consistent = false;
                    }
                }
                if (pair_consistent &&
                    ir::parse_ir_module_cache(ibytes.data(), ibytes.size(),
                                              dep_mod)) {
                    /* Por la misma funcion que el acierto del
                     * almacen comun. */
                    adopt_cached_artifact(pm, std::move(pr.module_),
                                          std::move(dep_mod));
                    // Seed del CAS global desde un HIT del cache
                    // por-path: asi el primer build con .vxir
                    // caliente (pero CAS frio) puebla el store
                    // global para otros proyectos/maquinas, no solo
                    // el build que compila desde cero.  El .vxi
                    // valido ya confirmo que corresponde al
                    // contenido actual -> cas_key es la clave
                    // correcta para este artefacto.
                    if (env.cache.cas && keys.cas_key_ok)
                        (void)env.cache.cas->put(
                            keys.cas_key, pack_module_artifact(vbytes, ibytes));
                    if (env.cache.verbose) {
                        std::ostringstream tmp;
                        tmp << "[vx-cache] hit: " << pm.canonical_path.str()
                            << "\n";
                        std::lock_guard<std::mutex> lk(*env.verbose_mtx);
                        std::cerr << tmp.str();
                    }
                    return true; // skip rest of compile
                }
            }
        }
    }
    if (env.cache.verbose) {
        std::ostringstream tmp;
        tmp << "[vx-cache] miss: " << pm.canonical_path.str() << "\n";
        std::lock_guard<std::mutex> lk(*env.verbose_mtx);
        std::cerr << tmp.str();
    }
    return false;
}

} // namespace

UnitCacheKeys unit_cache_keys(const UnitEnv &env, size_t i) {
    const std::vector<ProjectModuleWork> &work = *env.work;
    const ModuleLookup &lookup = *env.lookup;
    const ProjectModuleWork &pm = work[i];
    const bool is_root = (i + 1 == work.size());
    UnitCacheKeys keys;

    /* Lo que decide si un artefacto guardado sirve para esta compilacion.
     * Cada ingrediente, y por que esta, en `vx/project/module_cache_key.h`. */
    ModuleCacheKeyInput key_in;
    key_in.source = &pm.source;
    key_in.opts = env.opts;
    key_in.comptime_machine =
        (env.opts_modules->comptime_artifact != nullptr &&
         !env.opts_modules->comptime_artifact->empty()) ||
        !util::flag_text(util::FlagId::McPrebuilt).empty();
    key_in.hooks_source_fp = env.cache.hooks_source_fp;
    key_in.target_os = env.cache.target_os;
    key_in.target_arch = env.cache.target_arch;
    const ModuleCacheKey cache_key = module_cache_key(key_in);
    keys.source_hash = cache_key.source_hash;
    keys.target_suffix = cache_key.target_suffix;

    // ---- CAS global (content-addressed, cross-proyecto) ----
    // Clave de contenido del modulo (independiente de la ruta).  Se calcula
    // aqui para reusarla tambien en el write path (mas abajo).  Solo DEPS
    // (el root se ensambla, no se cachea como artefacto reusable).
    /* La MISMA clave de contenido sirve a dos cosas, y se calcula una vez:
     * a un dep le da su entrada en el almacen comun, y al RAIZ su identidad
     * para la cache de hechos.  El raiz no se guarda en el almacen, pero
     * los hechos SI son suyos: se producen sobre el modulo ya fusionado,
     * que es el raiz con todo lo que arrastra.  Que salgan de la misma
     * funcion es lo que impide que dos cosas que son la misma identidad se
     * separen.
     *
     * `root_facts_key` lo escribe SOLO la tarea del raiz y se lee cuando
     * todas han terminado, asi que no compite con nadie aunque los modulos
     * se compilen en paralelo. */
    if (pm.ast && (is_root || env.cache.cas)) {
        DepAbiHashes dep_hashes;
        for (const ImportRequest &req : lookup.imports_of(pm)) {
            const size_t dep = lookup.find(req);
            if (dep < work.size()) dep_hashes.push_back(work[dep].vxi.abi_hash);
        }
        std::sort(dep_hashes.begin(), dep_hashes.end());
        const uint64_t content_key =
            module_content_key(keys.source_hash, dep_hashes,
                               keys.target_suffix.str(), env.cache.cas_config_fp);
        if (is_root) {
            *env.root_facts_key = content_key;
        } else {
            keys.cas_key = content_key;
            keys.cas_key_ok = true;
        }
    }
    return keys;
}

bool serve_unit_from_cache(const UnitEnv &env, size_t i,
                           const UnitCacheKeys &keys) {
    ProjectModuleWork &pm = (*env.work)[i];
    const bool is_root = (i + 1 == env.work->size());
    if (serve_from_cas(env, pm, keys)) return true;

    // ---- M4: cache hit path ----
    // Solo se aplica a DEPS, no al root.  Verifica:
    //   1. Existe `<source>.vxi`.
    //   2. .vxi.source_hash == source_hash actual.
    //   3. Existe `<source>.vxir` para reusar el IR.
    // Si los 3 se cumplen, se skipea el recompile del dep.
    if (env.cache.enabled && !is_root)
        return serve_from_path_cache(env, pm, keys);
    return false;
}

void persist_unit(const UnitEnv &env, size_t i, const UnitCacheKeys &keys) {
    ProjectModuleWork &pm = (*env.work)[i];
    const bool is_root = (i + 1 == env.work->size());
    const CompileOptions &opts = *env.opts;
    // ---- M3: persistir .vxi + .vxir a disco para futuro cache ----
    if (!(env.cache.enabled && !is_root)) return;
    const ModuleCachePaths &paths =
        module_cache_paths(pm.canonical_path, keys.target_suffix);
    const std::string &vp = paths.vxi;
    const std::string &ip = paths.vxir;
    auto vbytes = vxi_emit(pm.vxi);
    //  M4.ext L.13: capturar el abi_hash recien calculado
    // por vxi_emit (lo escribio en offset 8 del header) para
    // que los modulos sucesores en topo order que importen este
    // puedan apuntarlo en su dep table.
    if (vbytes.size() >= 16) {
        uint64_t h = 0;
        for (int b = 0; b < 8; ++b) {
            h |= static_cast<uint64_t>(vbytes[8 + b]) << (b * 8);
        }
        pm.vxi.abi_hash = h;
    }
    //  M5.A L.17: escritura atomica (rename temp file).
    // El IR va PRIMERO y la interfaz DESPUES.  Cada fichero se
    // escribe de forma atomica, pero son dos ficheros que tienen que
    // corresponderse, y quien los lee entra por la interfaz: si esta
    // se publicara antes, otra compilacion simultanea podria
    // encontrarse la interfaz nueva junto al IR viejo y quedarse con
    // una mezcla de dos versiones.  Publicando el IR primero, ver la
    // interfaz nueva garantiza que su IR ya esta en disco.
    //
    // BugFix M.vxir-sd: persistir el modulo COMPLETO (functions +
    // static_data + globals) para que un dep cache-hit aporte sus
    // slots `code.s_*` al merge.  emit_ir_section (solo functions)
    // los perdia.
    auto ibytes = ir::emit_ir_module_cache_vec(pm.ir);
    (void)::fs::write_file_atomic(ip, ibytes);
    (void)::fs::write_file_atomic(vp, vbytes);
    /* Y queda apuntado que el intermedio de este modulo, TAL COMO ESTA
     * AHORA, ya esta en disco.  Es lo que permite que desalojarlo mas
     * tarde no cueste ni una escritura mas: el fichero que la cache
     * acaba de publicar es el mismo del que se recupera.
     *
     * Se apunta AQUI, en la linea siguiente a escribirlo, y no en otro
     * sitio: la afirmacion vale mientras nadie toque `pm.ir` entre las
     * dos cosas.  Quien anada una modificacion despues tiene que
     * limpiar esto. */
    pm.ir_cache_path = ip;
    // Poblar el CAS global (content-addressed) con el mismo par
    // (interfaz, IR).  Idempotente: la clave es el contenido.  Asi el
    // siguiente proyecto/maquina con esta misma stdlib hace hit sin
    // recompilar, sin importar en que ruta viva.
    if (env.cache.cas && keys.cas_key_ok)
        (void)env.cache.cas->put(keys.cas_key,
                                 pack_module_artifact(vbytes, ibytes));
    //  M5.C L.18: ademas del .vxi (interfaz) + .vxir (IR
    // serializado), emitir el .vel del dep solo (sin merge) para
    // que la libreria sea distribuible standalone.  El
    // consumidor puede tomar lib.vx + lib.vxi + lib.vel y
    // armar su .velb directamente con vm --asm-file lib.vel.
    /* APAGADO POR DEFECTO, y no es una micro-optimizacion.
     *
     * Producir este `.vel` obliga a llamar a `ir_emit_module` una
     * SEGUNDA vez sobre el mismo modulo -- la primera es la del IR
     * fusionado, mas abajo --, y emitir incluye asignar registros.  El
     * comentario de esa otra llamada lo dice: *"asignar registros y
     * escribir el texto .vel es el noventa por ciento del coste del
     * frontend"*.  O sea que cada funcion del proyecto se asignaba
     * registros dos veces.
     *
     * Medido sobre el proyecto de 21 modulos del banco de compilacion
     * (144k lineas): el banco de registros fisicos se construia 48.001
     * veces para 24.001 funciones -- exactamente dos por funcion --, y
     * la cuenta cuadraba por dos caminos independientes (el vector de
     * `Lane` y el de `ViewGeom`).
     *
     * Lo que se pierde al apagarlo es real y por eso hay valvula: sin
     * este fichero la dependencia no se puede repartir suelta, que era
     * su motivo (`lib.vx` + `lib.vxi` + `lib.vel` y armar el `.velb`
     * con `vm --asm-file lib.vel`).  Quien reparta modulos sueltos pone
     * `VESTA_DEP_VEL=1`; quien solo quiere su `.velb` no paga por una
     * emision que no va a usar.
     *
     * El `.velb` del proyecto NO cambia: este camino solo escribia un
     * artefacto lateral.  Por eso la bandera es `Speed` y no `Emitted`
     * -- no entra en la huella. */
    if (util::flag_present(util::FlagId::DepVel)) {
        ir::EmitOptions dep_emit_opts;
        dep_emit_opts.opt_level = ir::opt_level_from_int(opts.opt_level);
        dep_emit_opts.emit_debug = opts.emit_debug;
        // emit_stackmaps en su default (true): VSMP siempre presente.
        dep_emit_opts.module_name = pm.module_name.str();
        ir::EmitResult dep_eres = ir::ir_emit_module(pm.ir, dep_emit_opts);
        const std::string &dvel_path =
            module_cache_paths(pm.canonical_path, util::InternedName()).vel;
        if (dep_eres.ok) {
            std::vector<uint8_t> velb_bytes(dep_eres.vel_text.begin(),
                                            dep_eres.vel_text.end());
            (void)::fs::write_file_atomic(dvel_path, velb_bytes);
        }
        if (env.cache.verbose) {
            std::ostringstream tmp;
            tmp << "[vx-cache] wrote: " << vp << " (" << vbytes.size()
                << " B) + " << ip << " (" << ibytes.size() << " B) + "
                << dvel_path << " ("
                << (dep_eres.ok ? dep_eres.vel_text.size() : 0) << " B)\n";
            std::lock_guard<std::mutex> lk(*env.verbose_mtx);
            std::cerr << tmp.str();
        }
    } else if (env.cache.verbose) {
        std::ostringstream tmp;
        tmp << "[vx-cache] wrote: " << vp << " (" << vbytes.size() << " B) + "
            << ip << " (" << ibytes.size()
            << " B)  [sin .vel: VESTA_DEP_VEL apagado]\n";
        std::lock_guard<std::mutex> lk(*env.verbose_mtx);
        std::cerr << tmp.str();
    }
}

} // namespace vx
