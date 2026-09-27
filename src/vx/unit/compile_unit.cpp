/**
 * @file compile_unit.cpp
 * @brief Compilar UN modulo de un proyecto: las piezas, en su orden.
 *
 * Aqui solo va el ORDEN y los cortes.  Lo que hace cada paso vive en su
 * fichero de `src/vx/unit/`.
 */
#include "vx/unit/compile_unit.h"

#include "util/crash_report.h" // dejar dicho QUE modulo se esta compilando
#include "vx/comptime/macro_report.h"
#include "vx/diagram/source_diagrams.h"
#include "vx/project/module_work.h"
#include "vx/type_checker.h"

#include <iostream>
#include <mutex>
#include <sstream>

namespace vx {

void compile_unit(const UnitEnv &env, size_t i) {
    std::vector<ProjectModuleWork> &work = *env.work;
    auto &pm = work[i];
    /* Que fichero se esta compilando, para que una caida lo diga.
     *
     * Es el dato que mas tiempo ahorra de un informe de caida: una traza de
     * pila dice donde reviento -- quince marcos de plantillas -- y esto
     * dice sobre QUE.  Cuesta dos escrituras atomicas por modulo, y la
     * cadena vive en `work`, que dura mas que esta llamada. */
    const util::CrashContext crash_ctx("crash.stage.compiling",
                                       pm.canonical_path.c_str());
    const bool is_root = (i + 1 == work.size());
    if (env.verbose_compile) {
        std::ostringstream ln;
        ln << "[L" << (*env.levels)[i] << "][" << (i + 1) << "/"
           << work.size() << "] compiling " << pm.module_name.str()
           << (is_root ? " (root)" : "") << "...\n";
        std::lock_guard<std::mutex> lk(*env.verbose_mtx);
        std::cerr << ln.str();
    }

    const UnitCacheKeys keys = unit_cache_keys(env, i);
    if (serve_unit_from_cache(env, i, keys)) return;

    const std::vector<FlattenedNamespace> inline_namespaces =
        prepare_unit(env, i);

    std::vector<ImportRequest> imports;
    if (!inject_unit_imports(env, i, inline_namespaces, imports)) {
        pm.ok = false;
        return;
    }
    if (!check_unit(env, i, imports)) {
        pm.ok = false;
        return;
    }
    /* Las llamadas a `@Macro` que el comprobador resolvio, por la misma
     * funcion que el camino de fichero suelto. */
    collect_macro_expectations(*pm.tc, pm.macro_expectations);
    /* Los diagramas del arbol y de los tipos del raiz, en el mismo momento
     * que el camino de fichero suelto: tipado y todavia sin bajar.  El raiz
     * se compila solo en su nivel, asi que escribir en el resultado no
     * compite con nadie. */
    if (is_root) fill_source_diagrams(*pm.ast, *env.opts, *env.res);
    if (!lower_unit(env, i)) {
        pm.ok = false;
        return;
    }

    build_unit_interface(env, i, keys, imports);
    persist_unit(env, i, keys);

    if (pm.tc && pm.tc->inject_diferido()) {
        pm.inject_pending = true;
        pm.inject_code = pm.tc->asm_body_pending_code();
        pm.inject_arg = pm.tc->asm_body_pending_arg();
    }

    pm.ok = true;
}

} // namespace vx
