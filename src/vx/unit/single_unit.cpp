/**
 * @file single_unit.cpp
 * @brief Compilar un fichero suelto como un proyecto de un modulo.
 * @see vx/unit/single_unit.h
 */
#include "vx/unit/single_unit.h"

#include "vx/compiler.h"
#include "vx/parser.h" // get_aot_condcomp_target
#include "vx/project/module_imports.h"
#include "vx/project/module_work.h"
#include "vx/project/root_weaving.h"
#include "vx/unit/compile_unit.h"
#include "vx/unit/unit_env.h"
#include "vx/unit/unit_results.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace vx {

bool compile_single_unit(std::vector<ProjectModuleWork> &work,
                         const CompileOptions &opts, CompileResult &res) {
    ProjectModuleWork &root = work.back();

    /* Lo que un proyecto sabe de sus modulos, para uno solo: no hay a quien
     * importar.  Un `import` en un fichero suelto no encuentra su modulo y se
     * DICE -- antes se ignoraba y el fallo salia despues como un nombre no
     * declarado --. */
    const ModuleLookup lookup =
        build_module_lookup(work, AutoImportNs{}, std::string());
    const std::vector<int> levels(work.size(), 0);

    /* Lo que el raiz teje, por la misma pieza que el proyecto, y con el arbol
     * SIN aplanar: los nombres se calculan como quedaran. */
    RootWeaving weaving;
    if (!collect_root_weaving(work, res, weaving)) return false;

    // El objetivo de `@Target`, capturado aqui como en el proyecto.
    std::string target_os;
    std::string target_arch;
    get_aot_condcomp_target(target_os, target_arch);

    std::mutex verbose_mtx;
    uint64_t root_facts_key = 0;
    const std::string package_id;
    const std::vector<std::string> package_override;

    UnitEnv env;
    env.work = &work;
    env.lookup = &lookup;
    env.levels = &levels;
    env.opts = &opts;
    env.opts_modules = &opts;
    // Sin cache: un fichero suelto no deja artefactos por modulo.
    env.cache.enabled = false;
    env.cache.hooks_source_fp = weaving.hooks_source_fp;
    env.cache.target_os = &target_os;
    env.cache.target_arch = &target_arch;
    env.root = weaving.view();
    env.target_skipped = &root.ast->target_skipped;
    env.project_package_id = &package_id;
    env.module_package_override = &package_override;
    env.verbose_mtx = &verbose_mtx;
    env.res = &res;
    env.root_facts_key = &root_facts_key;

    compile_unit(env, work.size() - 1);
    return gather_unit_results(work, res);
}

} // namespace vx
