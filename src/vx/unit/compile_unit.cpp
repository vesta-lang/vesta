/**
 * @file compile_unit.cpp
 * @brief Compilar UN modulo de un proyecto: las piezas, en su orden.
 *
 * Aqui solo va el ORDEN y los cortes.  Lo que hace cada paso vive en su
 * fichero de `src/vx/unit/`.
 */
#include "vx/unit/compile_unit.h"

#include "util/crash_report.h" // dejar dicho QUE modulo se esta compilando
#include "vx/compiler.h"
#include "vx/comptime/comptime_values.h"
#include "vx/comptime/macro_report.h"
#include "vx/diagram/source_diagrams.h"
#include "vx/project/module_work.h"
#include "vx/type_checker.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>

namespace vx {

namespace {

/// El reloj con que se miden las fases de un modulo.
using PhaseClock = std::chrono::steady_clock;

/**
 * @brief Microsegundos desde @p mark, y mueve la marca a ahora.
 * @param mark La marca de la fase anterior; queda en el instante actual.
 * @return Lo que duro la fase.
 */
long close_phase_us(PhaseClock::time_point &mark) {
    const PhaseClock::time_point now = PhaseClock::now();
    const long us = static_cast<long>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - mark)
            .count());
    mark = now;
    return us;
}

} // namespace

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

    /* Desde aqui, la fase de TIPOS: crear el comprobador con lo que traen los
     * imports, ejecutarlo y lo que se saca de el.  Mismas fronteras que en el
     * camino de fichero suelto, que las publica tal cual. */
    PhaseClock::time_point mark = PhaseClock::now();
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
    /* Y los valores comptime del documento, si el editor los pidio: el
     * documento es el raiz. */
    if (is_root && env.opts->dump_comptime_values)
        collect_comptime_values(*pm.tc, env.res->comptime_values);
    pm.types_us = close_phase_us(mark);

    if (!lower_unit(env, i)) {
        pm.ok = false;
        return;
    }
    pm.lowering_us = close_phase_us(mark);

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
