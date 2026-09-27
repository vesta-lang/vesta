/**
 * @file unit_results.cpp
 * @brief Juntar en el resultado lo que dejo la compilacion de cada modulo.
 * @see vx/unit/unit_results.h
 */
#include "vx/unit/unit_results.h"

#include "util/fnv.h" // util::hash_combine
#include "vx/compiler.h"
#include "vx/project/module_work.h"

namespace vx {

bool gather_unit_results(std::vector<ProjectModuleWork> &work,
                         CompileResult &res) {
    /* Despues de compilar y no dentro: los modulos se compilan en paralelo y
     * el resultado es uno solo.  En orden de indice, que ademas hace que dos
     * compilaciones del mismo fuente den el mismo texto y la misma clave. */
    bool any_failed = false;
    for (ProjectModuleWork &pm : work) {
        for (const auto &d : pm.diags.all())
            res.diagnostics.emit(d);
        /* "Aporto algo" se mide por el TEXTO o por los NOMBRES: un modulo cuyo
         * conjunto son solo constantes comptime trae nombres con el texto
         * vacio, y mirando solo el texto se perdian -- y con ellos el criterio
         * de pertenencia al emitir el artefacto. */
        if (!pm.comptime_unit_source.empty() ||
            !pm.comptime_unit_names.empty()) {
            res.comptime_unit_source += pm.comptime_unit_source;
            res.comptime_unit_names.insert(res.comptime_unit_names.end(),
                                           pm.comptime_unit_names.begin(),
                                           pm.comptime_unit_names.end());
            res.comptime_unit_hash =
                util::hash_combine(res.comptime_unit_hash, pm.comptime_unit_hash);
        }
        res.comptime_unit_not_collected.insert(
            res.comptime_unit_not_collected.end(),
            pm.comptime_unit_not_collected.begin(),
            pm.comptime_unit_not_collected.end());
        /* Lo que se informa de cada `@Macro`: el servidor de lenguaje lo
         * ensena, y sin sumarlo aqui un documento con `import` no ensenaba
         * nada. */
        res.macro_expectations.insert(res.macro_expectations.end(),
                                      pm.macro_expectations.begin(),
                                      pm.macro_expectations.end());
        res.macro_skip_reasons.insert(res.macro_skip_reasons.end(),
                                      pm.macro_skips.begin(),
                                      pm.macro_skips.end());
        /* La huella del mapa del modulo en el grafo de depuracion, AQUI y no
         * mas tarde: viene en su `.vxi` -- lo escribio la emision, o lo trajo
         * la cache --, y el `.vxi` se suelta en cuanto esto termina.  Leerla
         * despues daba siempre cero, y el mapa del artefacto no citaba ningun
         * modulo: su grafo se quedaba sin raiz. */
        pm.vxdbg_module_map =
            vxdbg::ContentHash{pm.vxi.vxdbg_map_lo, pm.vxi.vxdbg_map_hi};
        if (!pm.ok) any_failed = true;
    }
    if (any_failed) res.ok = false;
    return !any_failed;
}

} // namespace vx
