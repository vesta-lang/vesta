/**
 * @file unit_check.cpp
 * @brief Comprobar los tipos de un modulo y avisar de los imports del raiz que
 *        no se usan.
 */
#include "vx/unit/compile_unit.h"

#include "vx/project/module_work.h"
#include "vx/type_checker.h"

#include <string>

namespace vx {

bool check_unit(const UnitEnv &env, size_t i,
                const std::vector<ImportRequest> &imports) {
    ProjectModuleWork &pm = (*env.work)[i];
    const bool is_root = (i + 1 == env.work->size());

    if (!pm.tc->run()) return false;

    //  M.L26: warning de imports sin usar.  Tras el check, el
    // TypeChecker tiene un set de nombres referenciados.  Cada
    // `import "lib" only A, B, C;` declara nombres en el scope
    // global; si alguno no aparece en @c referenced_names() , el
    // usuario lo importo pero no lo uso -> warning suave (NO error;
    // imports pueden documentar API surface intencionalmente).
    // Solo aplicable al root (no a deps; sus imports solo importan
    // si el dep luego los reexporta, pero el reexport plain no esta
    // soportado todavia en MVP).  Cualquier modulo con @c warning
    // ya promovido se cubrira en M.reexport.
    if (is_root) {
        const auto &refs = pm.tc->referenced_names();
        for (const auto &req : imports) {
            if (req.is_plain) {
                // Plain `import "x";` registra @c x como Symbol::Namespace.
                // Si el namespace alias nunca se accedio, warning.
                if (refs.find(req.local_name) == refs.end()) {
                    std::string msg = "import '";
                    msg += req.module_name;
                    if (!req.local_name.empty() &&
                        req.local_name != req.module_name) {
                        msg += "' as '";
                        msg += req.local_name;
                    }
                    msg += "' no se usa";
                    pm.diags.warning(req.loc, std::move(msg));
                }
            } else {
                // `only A, B`: chequear cada A, B individualmente.
                for (const auto &os : req.only_symbols) {
                    const std::string &local =
                        os.rename.empty() ? os.name : os.rename;
                    if (refs.find(local) == refs.end()) {
                        std::string msg = "simbolo importado '";
                        msg += local;
                        msg += "' de '";
                        msg += req.module_name;
                        msg += "' no se usa";
                        pm.diags.warning(req.loc, std::move(msg));
                    }
                }
            }
        }
    }
    return true;
}

} // namespace vx
