/**
 * @file unit_lower.cpp
 * @brief Bajar un modulo ya comprobado a IR, con lo que el raiz teje en el, y
 *        emitir su grafo de depuracion.
 */
#include "vx/unit/compile_unit.h"

#include "util/crono_tramo.h" // el tramo de la bajada en el informe de tiempos
#include "util/env_flags.h"
#include "vx/compiler.h"
#include "vx/lowering.h"
#include "vx/project/module_work.h"
#include "vx/vxdbg_emit.h"
#include "vxdbg/roots.h" // vxdbg::SourceExtent

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace vx {

namespace {

/**
 * @brief Pasa al lowering del raiz los sustitutos que decide el programa: los
 *        `@HelperOverride` y los del `string` built-in y del monitor.
 * @param env Entorno del proyecto.
 * @param pm  El modulo raiz.
 * @param lo  Su lowering.
 * @return false si los sustitutos chocan o vienen a medias: el error ya esta
 *         dicho y el modulo no se baja.
 */
bool apply_root_overrides(const UnitEnv &env, ProjectModuleWork &pm,
                          Lowering &lo) {
    CompileResult &res = *env.res;
    const auto &aot_helper_override_syms = *env.root.helper_overrides;
    auto itmc = aot_helper_override_syms.find("memcpy");
    if (itmc != aot_helper_override_syms.end())
        lo.set_memcpy_override(itmc->second);
    auto itsc = aot_helper_override_syms.find("strcmp");
    if (itsc != aot_helper_override_syms.end())
        lo.set_strcmp_override(itsc->second);
    auto itsl = aot_helper_override_syms.find("strlen");
    if (itsl != aot_helper_override_syms.end())
        lo.set_strlen_override(itsl->second);
    /* Y los sustitutos del `string` built-in y del monitor, por el
     * MISMO barrido que usa el camino de fichero suelto.
     *
     * Este camino no los tenia, y eso no daba un error: un
     * `@StringConcat` compilaba y el `+` seguia yendo al concat del
     * lenguaje, con el override emitido al lado sin que lo llamara
     * nadie.  Saltaba en cuanto un fichero pasaba a compilarse como
     * proyecto por cualquier otro motivo -- por ejemplo por traer el
     * asignador de la stdlib --, o sea lejos de donde se escribio. */
    /* Si choca, se corta AQUI, como en el camino de fichero suelto: bajar el
     * modulo con uno de los dos candidatos elegido por el orden en que estan
     * escritos seria dar por buena una pregunta sin respuesta. */
    if (!collect_string_sync_overrides(*pm.ast, *pm.tc, res)) return false;
    lo.set_string_op_overrides(res.string_concat_override,
                               res.string_eq_override);
    lo.set_sync_impl_overrides(res.sync_enter_override,
                               res.sync_exit_override);
    return true;
}

/**
 * @brief Emite el grafo de conocimiento del modulo y guarda en el su huella.
 * @param env Entorno del proyecto.
 * @param pm  El modulo.
 * @param lo  Su lowering, ya ejecutado.
 */
void emit_unit_vxdbg(const UnitEnv &env, ProjectModuleWork &pm, Lowering &lo) {
    const CompileOptions &opts = *env.opts;
    // Grafo de conocimiento del programa, por modulo.  Cada uno aporta sus
    // tipos y los simbolos que emitio; el mapa del artefacto se compone
    // despues con lo de todos, porque el ejecutable final los contiene a
    // todos y una direccion suya puede caer en cualquiera.
    VxdbgEmitStats st;
    std::string dbg_err;
    /* Se LLEVAN, no se copian: ver el mismo sitio en `compiler.cpp`.
     * Aqui pesa mas todavia, porque esto corre una vez por MODULO. */
    std::vector<vxdbg::SourceExtent> spans;
    auto emitted = lo.take_emitted_spans();
    spans.reserve(emitted.size());
    for (auto &e : emitted)
        spans.push_back({std::move(e.symbol), e.line, e.column, e.length});
    if (!emit_vxdbg_source(*pm.tc, lo.emitted_symbols(), std::move(spans),
                           pm.canonical_path.str(), pm.source, opts.vxdbg_dir,
                           st, dbg_err)) {
        std::cerr << "[vxdbg] no se pudo emitir " << pm.canonical_path.str()
                  << ": " << dbg_err << "\n";
    }
    pm.vxdbg_symbols = std::move(st.symbol_links);
    /* La huella del mapa de este modulo viaja en su `.vxi`.  Es lo que
     * permite que, cuando el modulo se sirva desde cache y no se baje,
     * siga aportando su grafo al artefacto: sin esto sus simbolos no
     * llegaban al mapa y su grafo se quedaba sin sostener. */
    pm.vxi.vxdbg_map_lo = st.module_map.lo;
    pm.vxi.vxdbg_map_hi = st.module_map.hi;
    /* Y sus tramos de fuente, que no viajan en el `.vxi`: solo los tiene el
     * modulo que se bajo en esta compilacion.  El artefacto publica los del
     * raiz, que es lo que subraya la traza. */
    pm.vxdbg_span_map = st.span_map;
}

} // namespace

bool lower_unit(const UnitEnv &env, size_t i) {
    ProjectModuleWork &pm = (*env.work)[i];
    const bool is_root = (i + 1 == env.work->size());
    const CompileOptions &opts = *env.opts;

    Lowering lo(*pm.ast, *pm.tc, pm.diags);
    lo.avisar_asm_opaco_ = opts.emit_ir_preopt;
    //  AOT multi-modulo: propagar POO/strings nativos a TODOS los
    // modulos del proyecto (no solo al single-file).  Sin esto los deps
    // se bajaban en modo Full (GC) y el IR mergeado no era AOT-compatible.
    lo.set_native_poo(opts.native_poo);
    // Ancho del target para el inline-asm que GENERA el lowering (el
    // detector de features de CPU, los helpers @Naked...).  El camino de
    // fichero suelto ya lo propagaba; este no, asi que al compilar un
    // proyecto para x86-32 se emitian registros de 64 bits y el ensamblado
    // fallaba, tumbando la funcion entera al interprete.
    lo.set_asm_target_bits(opts.asm_target_bits);
    /* El ancho SIMD de `--float-isa`, que la vectorizacion del bajado usa en
     * modo nativo.  Solo lo pasaba el camino de fichero suelto: un proyecto
     * AOT vectorizaba siempre a 16 bytes, pidiera lo que pidiera.  Entra en
     * la clave de cache del modulo (`module_cache_key`). */
    lo.set_aot_vec_width(opts.aot_vec_width);
    lo.set_aot_auto_vec(opts.aot_auto_vec);
    // CPU dispatch Inc 5b: aplicar los @HelperOverride agregados (root +
    // imports, ya resueltos por precedencia en el pre-pase) SOLO al
    // modulo ROOT, que es quien emite __vx_memcpy_init / __vx_strdisp_init.
    // El fp de cada init apunta entonces a la fn del override (que puede
    // vivir en un modulo importado; su simbolo se resuelve en el IR
    // mergeado via el reloc fnsym del LABEL_ADDR).
    if (is_root && !apply_root_overrides(env, pm, lo)) return false;
    /* Solo para el editor: bajar tambien las funciones comptime para poder
     * inspeccionarlas.  Del documento, que es el raiz. */
    if (is_root) lo.set_emit_comptime_fns(opts.emit_comptime_fns);
    if (!opts.instrument_mode.empty() && opts.instrument_mode != "none") {
        lo.set_instrument_mode(opts.instrument_mode);
    }
    /* Los `@Hook` del raiz alcanzan a TODOS los modulos: es lo que hace
     * posible medir la stdlib declarando el gancho una sola vez.  Al raiz
     * no se le pasan -- ya los tiene en su propio AST y se recogeria dos
     * veces el mismo gancho. */
    if (!is_root && !env.root.hooks->empty())
        lo.set_root_hooks(*env.root.hooks, *env.root.no_instrument);
    /* Los contadores van a TODOS, raiz incluido: el raiz recoge sus
     * ganchos de su propio arbol, y sin contador creeria que no se
     * instalaron en ningun sitio -- justo el modulo desde el que se ve
     * peor, porque un selector como `"std.*"` no casa nada ahi. */
    if (!env.root.hook_counters->empty())
        lo.set_hook_counters(*env.root.hook_counters);
    const std::string &mod_name = pm.module_name.str();
    {
        /* La bajada de verdad, con su propio tramo en el informe de tiempos:
         * sin el, su coste se leeria dentro de la fase que la contenga. */
        util::CronoTramo t_("lower:run", util::flag_on(util::FlagId::Times));
        if (!lo.run(pm.ir, mod_name)) return false;
    }
    /* Los `@Macro` que no fueron a la maquina de compilacion, y por que.  Se
     * juntan con los de los demas modulos al acabar. */
    pm.macro_skips = lo.macro_skip_reasons();
    /* La autocomprobacion del intermedio recien bajado, como en el camino de
     * fichero suelto: sin ella, un modulo de proyecto solo se verificaba ya
     * fusionado, donde un fallo no dice de que modulo vino. */
    ir::ir_verify_if_asked(pm.ir, "lowered", pm.canonical_path.str());

    /* De que fichero salio cada funcion.  Aqui y no mas tarde: este es el
     * ultimo punto en que el modulo y su fuente se ven a la vez -- despues
     * se fusiona con los demas y dentro conviven funciones de muchos --.
     *
     * Este camino, el de PROYECTO, es justamente el que lo necesita: el de
     * fichero suelto tiene un fichero y ya. */
    pm.ir.assign_source_file(pm.canonical_path.str());

    emit_unit_vxdbg(env, pm, lo);

    // -ffp-contract=off (CLI, per-modulo): fuerza IEEE estricto (sin FMA)
    // en cada funcion del modulo.  Mismo criterio que compile_vx_source; se
    // aplica aqui (misma TU que el optimizer del proyecto) para no depender
    // del global mutable duplicado entre vm.exe/DLL/vmcore.
    if (!opts.fp_contract) {
        for (auto &fn : pm.ir.functions)
            fn.fp_contract = false;
    }
    return true;
}

} // namespace vx
