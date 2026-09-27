/**
 * @file root_weaving.cpp
 * @brief Lo que el raiz teje en todo el programa.
 * @see vx/project/root_weaving.h
 */
#include "vx/project/root_weaving.h"

#include "util/fnv.h"
#include "vx/ast.h"
#include "vx/compiler.h"
#include "vx/helper_override.h"
#include "vx/module/namespace_names.h" // namespace_member_symbol
#include "vx/project/module_names.h"   // module_member_symbol
#include "vx/project/module_work.h"

#include <atomic>
#include <memory>
#include <unordered_set>

namespace vx {

namespace {

/**
 * @brief Recoge los `@HelperOverride` de todos los modulos, con precedencia
 *        del raiz.
 *
 * Tienen que resolverse ANTES de bajar el raiz, que es quien genera los
 * inicializadores (`__vx_memcpy_init`, `__vx_strdisp_init`) que apuntan cada
 * puntero a la funcion sustituta.  El nombre se captura como quedara en el
 * IR fusionado: los de un modulo que no es el raiz llevan su prefijo.
 *
 * @param work Los modulos; el raiz el ultimo.
 * @param res  Recibe el error de un choque.
 * @param out  destino -> simbolo.
 * @return false si dos del mismo nivel sustituyen el mismo ayudante.
 */
bool collect_helper_overrides(std::vector<ProjectModuleWork> &work,
                              CompileResult &res,
                              std::unordered_map<std::string, std::string> &out) {
    // Los destinos cuyo sustituto vino del raiz, para la precedencia.
    std::unordered_set<std::string> from_root;
    for (size_t mi = 0; mi < work.size(); ++mi) {
        ProjectModuleWork &pm = work[mi];
        if (!pm.ast) continue; // un acierto de cache con el AST conservado sirve
        const bool is_root = (mi + 1 == work.size());
        for (auto &decl : pm.ast->decls) {
            if (!decl || decl->kind != ast::NodeKind::FunctionDecl) continue;
            auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
            if (fd->helper_override_target.empty()) continue;
            const std::string &tgt = fd->helper_override_target;
            if (!check_helper_override(*fd, pm.diags)) continue;
            /* El simbolo como quedara: fuera del raiz, las funciones de nivel
             * superior se renombran con el prefijo del modulo DENTRO de
             * `compile_unit`, que todavia no ha corrido. */
            const std::string sym_name =
                is_root ? fd->name
                        : module_member_symbol(pm.module_name.str(), fd->name);
            auto existing = out.find(tgt);
            if (existing == out.end()) {
                out[tgt] = sym_name;
                if (is_root) from_root.insert(tgt);
                continue;
            }
            const bool prev_from_root = from_root.count(tgt) != 0;
            if (is_root && !prev_from_root) {
                // El raiz pisa al import: es codigo directo del programa.
                existing->second = sym_name;
                from_root.insert(tgt);
            } else if (!is_root && prev_from_root) {
                // Ya estaba el del raiz: el del import no cuenta.
            } else {
                // Dos del mismo nivel: no hay a quien dar la razon.
                res.ok = false;
                res.diagnostics.diag(fd->loc, DiagLevel::ERR, "VX4016",
                                     {tgt, existing->second, sym_name});
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief Recoge los `@Hook` y `@NoInstrument` de unas declaraciones, entrando
 *        en los namespaces.
 *
 * Aqui el arbol todavia no esta aplanado, asi que un fichero que empieza por
 * `namespace app.principal;` tiene UNA declaracion arriba y sus funciones
 * cuelgan de ella: sin entrar no se encontraba ningun gancho, y el programa
 * compilaba sin medir nada.  Los namespaces se anidan, de ahi la recursion.
 *
 * El nombre que se guarda es el aplanado -- `app__principal__al_entrar` --,
 * que es por el que se le llama desde otro modulo; se calcula aparte y no se
 * escribe en el nodo, porque el aplanado de verdad pasara despues por aqui.
 *
 * @param decls Las declaraciones.
 * @param ns    El namespace en que estan, con puntos; vacio arriba del todo.
 * @param out   Donde se anaden.
 */
void collect_hooks(const std::vector<std::unique_ptr<ast::Node>> &decls,
                   const std::string &ns, RootWeaving &out) {
    for (const auto &decl : decls) {
        if (!decl) continue;
        if (decl->kind == ast::NodeKind::NamespaceDecl) {
            const auto *nd = static_cast<const ast::NamespaceDecl *>(decl.get());
            collect_hooks(nd->decls, ns.empty() ? nd->name : ns + "." + nd->name,
                          out);
            continue;
        }
        if (decl->kind != ast::NodeKind::FunctionDecl) continue;
        auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
        const std::string flat =
            ns.empty() ? fd->name : namespace_member_symbol(ns, fd->name);
        if (!fd->hook_point.empty()) out.hooks.push_back({fd, flat});
        if (fd->is_no_instrument) out.no_instrument.push_back(flat);
    }
}

/**
 * @brief Mezcla el texto de @p s en @p h.
 * @param h Acumulador.
 * @param s Texto.
 * @return El acumulador actualizado.
 */
uint64_t mix_text(uint64_t h, const std::string &s) {
    return util::fnv_bytes(h, s.data(), s.size());
}

/**
 * @brief Huella de lo que el tejido cambia: punto, selector, nombre de
 *        llamada y campos pedidos de cada gancho.
 *
 * La usan las dos caches -- la clave del CAS y la de los artefactos junto al
 * fuente --, que con criterios distintos acabarian una invalidando y la otra
 * no.  El CUERPO del gancho no entra: cambiarlo cambia SU modulo, y de eso se
 * encarga el hash del fuente.
 *
 * @param hooks Los ganchos del raiz.
 * @return La huella, o 0 sin ganchos.
 */
uint64_t hooks_fingerprint(const std::vector<RootHook> &hooks) {
    if (hooks.empty()) return 0;
    uint64_t h = util::kFnvOffset;
    for (const RootHook &rh : hooks) {
        if (!rh.first) continue;
        h = mix_text(h, rh.first->hook_point);
        h = mix_text(h, rh.first->hook_selector);
        h = mix_text(h, rh.second);
        for (const auto &pd : rh.first->params)
            if (pd) h = mix_text(h, pd->name);
    }
    return h;
}

} // namespace

RootWeavingEnv RootWeaving::view() const {
    RootWeavingEnv env;
    env.helper_overrides = &helper_overrides;
    env.hooks = &hooks;
    env.no_instrument = &no_instrument;
    env.hook_counters = &hook_counters;
    return env;
}

bool collect_root_weaving(std::vector<ProjectModuleWork> &work,
                          CompileResult &res, RootWeaving &out) {
    if (!collect_helper_overrides(work, res, out.helper_overrides))
        return false;
    /* Los ganchos, solo del raiz -- la misma regla que `@Provides` --: medir
     * el programa entero lo decide el programa, no una biblioteca que use un
     * perfilador.  Se recogen AQUI, antes de bajar nada, porque los demas
     * modulos se bajan primero. */
    if (!work.empty() && work.back().ast)
        collect_hooks(work.back().ast->decls, std::string(), out);
    /* Un contador por gancho, compartido: cada modulo teje por su cuenta y
     * solo la SUMA dice si el gancho llego a alguna parte. */
    for (const RootHook &rh : out.hooks)
        out.hook_counters[rh.second] = std::make_shared<std::atomic<size_t>>(0);
    out.hooks_source_fp = hooks_fingerprint(out.hooks);
    return true;
}

} // namespace vx
