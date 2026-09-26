/**
 * @file module_imports.cpp
 * @brief Implementacion del modelo de imports del proyecto.
 * @see vx/project/module_imports.h
 */
#include "vx/project/module_imports.h"

#include "vx/module/vxi_format.h"   // vxi_hash_de_simbolos
#include "vx/project/module_work.h" // ProjectModuleWork

#include <unordered_set>

namespace vx {

namespace {

/**
 * @brief Recoge los ImportDecl de una lista de decls, entrando en los
 *        namespaces.
 * @param decls Las decls.
 * @param out   Donde se anaden, en orden de declaracion.
 */
void gather_import_decls(const std::vector<std::unique_ptr<ast::Node>> &decls,
                         std::vector<const ast::ImportDecl *> &out) {
    for (const auto &d : decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::ImportDecl)
            out.push_back(static_cast<const ast::ImportDecl *>(d.get()));
        else if (d->kind == ast::NodeKind::NamespaceDecl)
            gather_import_decls(
                static_cast<const ast::NamespaceDecl *>(d.get())->decls, out);
    }
}

/**
 * @brief El primer namespace que declara un modulo, o nulo.
 * @param pm El modulo.
 * @return Su NamespaceDecl de nivel superior.
 */
const ast::NamespaceDecl *first_namespace(const ProjectModuleWork &pm) {
    if (!pm.ast) return nullptr;
    for (const auto &d : pm.ast->decls)
        if (d && d->kind == ast::NodeKind::NamespaceDecl) {
            const auto *nd = static_cast<const ast::NamespaceDecl *>(d.get());
            if (!nd->name.empty()) return nd;
        }
    return nullptr;
}

} // namespace

size_t ModuleLookup::find(const ImportRequest &req) const {
    if (req.by_namespace && !req.ns_path.empty()) {
        const auto itn = by_ns.find(req.ns_path);
        if (itn != by_ns.end()) return itn->second;
    }
    return find_by_name(req.module_name);
}

size_t ModuleLookup::find(const VxiModule::DepRecord &dep) const {
    if (!dep.ns.empty()) {
        const auto itn = by_ns.find(dep.ns);
        if (itn != by_ns.end()) return itn->second;
    }
    return find_by_name(dep.name);
}

size_t ModuleLookup::find_by_name(const std::string &name) const {
    const auto itd = by_name.find(name);
    return itd != by_name.end() ? itd->second : kNoModule;
}

const std::string &ModuleLookup::namespace_of(size_t idx) const {
    static const std::string kNone;
    return idx < module_ns.size() ? module_ns[idx] : kNone;
}

const ModuleIndices *
ModuleLookup::modules_of_namespace(const std::string &ns) const {
    const auto it = ns_to_modules.find(ns);
    return it != ns_to_modules.end() ? &it->second : nullptr;
}

std::vector<ImportRequest>
ModuleLookup::imports_of(const ProjectModuleWork &pm) const {
    if (!pm.ast) return {};
    return collect_imports(*pm.ast, &ns_to_modname, &auto_imports,
                           auto_import_owner_dir, pm.canonical_path.str());
}

ModuleLookup build_module_lookup(const std::vector<ProjectModuleWork> &work,
                                 AutoImportNs auto_imports,
                                 std::string auto_owner_dir) {
    ModuleLookup lk;
    lk.auto_imports = std::move(auto_imports);
    lk.auto_import_owner_dir = std::move(auto_owner_dir);
    lk.module_ns.resize(work.size());
    for (size_t i = 0; i < work.size(); ++i) {
        const ProjectModuleWork &pm = work[i];
        // `emplace` conserva el primero: con homonimos gana el de menor
        // indice, el mismo criterio de siempre.
        lk.by_name.emplace(pm.module_name.str(), i);
        if (!pm.ast) continue;
        /* El primer namespace del modulo lo identifica; los demas indices
         * miran TODOS los que declara. */
        if (const ast::NamespaceDecl *nd = first_namespace(pm)) {
            lk.by_ns.emplace(nd->name, i);
            lk.module_ns[i] = nd->name;
        }
        for (const auto &d : pm.ast->decls) {
            if (!d || d->kind != ast::NodeKind::NamespaceDecl) continue;
            const auto *ns = static_cast<const ast::NamespaceDecl *>(d.get());
            if (ns->name.empty()) continue;
            lk.ns_to_modname.emplace(ns->name, pm.module_name.str());
            ModuleIndices &mods = lk.ns_to_modules[ns->name];
            if (mods.empty() || mods.back() != i) mods.push_back(i);
        }
    }
    return lk;
}

std::vector<ImportRequest> collect_imports(
    const ast::ModuleNode &mod,
    const std::unordered_map<std::string, std::string> *ns_to_modname,
    const AutoImportNs *auto_imports, const std::string &owner_dir,
    const std::string &self_path) {
    std::vector<ImportRequest> out;
    std::vector<const ast::ImportDecl *> imports;
    gather_import_decls(mod.decls, imports);
    for (const auto *im : imports) {
        ImportRequest req;
        req.by_namespace = im->by_namespace;
        if (im->by_namespace) {
            // `import a.b.c;`: el `path` es el namespace punteado.  Se traduce
            // al nombre del modulo que lo declara para reusar la maquinaria de
            // imports por ruta; si no esta, se deja el punteado y la busqueda
            // lo dira mas adelante.
            req.ns_path = im->path;
            req.module_name = im->path;
            if (ns_to_modname) {
                auto it = ns_to_modname->find(im->path);
                if (it != ns_to_modname->end()) req.module_name = it->second;
            }
        } else {
            // Por ruta: el modulo es el ultimo segmento.
            const size_t slash = im->path.find_last_of('/');
            req.module_name = (slash == std::string::npos)
                                  ? im->path
                                  : im->path.substr(slash + 1);
        }
        req.local_name = im->alias.empty() ? req.module_name : im->alias;
        req.only_symbols.reserve(im->only_symbols.size());
        for (const auto &os : im->only_symbols)
            req.only_symbols.push_back({os.name, os.rename});
        // Llano = sin `only` Y sin glob: registra el namespace en vez de
        // inyectar.  `only *` inyecta todos los publicos, asi que no lo es.
        req.only_all = im->only_all;
        req.is_plain = im->only_symbols.empty() && !im->only_all;
        req.is_public_reexport = im->is_public_reexport;
        req.loc = im->loc;
        out.push_back(std::move(req));
    }
    /* Y lo que el manifiesto declare auto-importable.
     *
     * Reservar memoria no se pide con un `import`: se escribe `new` o
     * `malloc<T>(n)`.  Pero quien lo atiende es una PLANTILLA, y una plantilla
     * hay que verla para instanciarla, asi que tiene que estar en el ambito de
     * quien reserva aunque su autor no escriba nada.  Que modulos son sale de
     * un DATO -- la lista del manifiesto --: en el compilador no hay ningun
     * nombre de modulo.
     *
     * El paquete que DECLARA la auto-importacion no se la aplica a si mismo:
     * los modulos de los que el asignador depende -- los tipos, los atomicos
     * -- se pedirian unos a otros y el grafo se cerraria en ciclo.  El
     * criterio es la frontera del paquete: dentro se escribe el `import` a
     * mano, como cualquier otra dependencia. */
    const bool inside_owner =
        !owner_dir.empty() && self_path.size() > owner_dir.size() &&
        self_path.compare(0, owner_dir.size(), owner_dir) == 0;
    if (auto_imports == nullptr || auto_imports->empty() || inside_owner)
        return out;
    for (const std::string *nsp : *auto_imports) {
        if (nsp == nullptr || nsp->empty()) continue;
        const std::string &ns = *nsp;
        bool already = false;
        for (const auto &r : out)
            if (r.ns_path == ns || r.module_name == ns) already = true;
        if (already) continue;
        ImportRequest req;
        req.by_namespace = true;
        req.ns_path = ns;
        req.module_name = ns;
        /* EXACTAMENTE como un `import std.alloc;` escrito a mano: LLANO, que
         * es el que registra el namespace y con el las PLANTILLAS del modulo.
         * `only *` se expande sobre la lista de SIMBOLOS, y una plantilla no
         * esta ahi -- no emite simbolo, lo emiten sus instancias --; y en un
         * import escrito las dos formas son EXCLUYENTES, asi que ponerlas
         * juntas creaba un estado que no ocurre nunca. */
        req.only_all = false;
        req.is_plain = true;
        if (ns_to_modname != nullptr) {
            auto it = ns_to_modname->find(ns);
            if (it != ns_to_modname->end() && !it->second.empty())
                req.module_name = it->second;
        }
        /* Y el nombre local es el del MODULO ya resuelto, como en el escrito:
         * dejarlo en el namespace puntuado lo registraba bajo un nombre que
         * despues nadie busca. */
        req.local_name = req.module_name;
        out.push_back(std::move(req));
    }
    return out;
}

uint64_t used_surface_hash(const VxiModule &dep_vxi, const ImportRequest &req) {
    if (req.is_plain || req.only_all || req.is_public_reexport ||
        req.only_symbols.empty()) {
        return dep_vxi.abi_hash;
    }
    std::vector<std::string> names;
    names.reserve(req.only_symbols.size());
    for (const auto &e : req.only_symbols)
        names.push_back(e.name);
    return vxi_hash_de_simbolos(dep_vxi, names);
}

ModuleIndices transitive_dependencies(const ModuleLookup &lookup,
                                      const std::vector<ProjectModuleWork> &work,
                                      const std::vector<ImportRequest> &imports) {
    ModuleIndices order;
    std::vector<bool> seen(work.size(), false);
    for (const auto &req : imports) {
        const size_t idx = lookup.find(req);
        if (idx >= work.size() || seen[idx]) continue;
        seen[idx] = true;
        order.push_back(idx);
    }
    // En anchura: `order` crece mientras se recorre.
    for (size_t qi = 0; qi < order.size(); ++qi) {
        for (const auto &de : work[order[qi]].vxi.deps) {
            const size_t idx = lookup.find(de);
            if (idx >= work.size() || seen[idx]) continue;
            seen[idx] = true;
            order.push_back(idx);
        }
    }
    return order;
}

std::vector<int> compute_module_levels(const std::vector<ProjectModuleWork> &work,
                                       const ModuleLookup &lookup) {
    std::vector<int> levels(work.size(), 0);
    // `work` ya esta en orden topologico: cada dependencia tiene su nivel.
    for (size_t i = 0; i < work.size(); ++i) {
        int max_dep_level = -1;
        for (const auto &req : lookup.imports_of(work[i])) {
            const size_t dep_idx = lookup.find(req);
            if (dep_idx >= work.size()) continue;
            if (levels[dep_idx] > max_dep_level) max_dep_level = levels[dep_idx];
        }
        levels[i] = max_dep_level + 1; // -1 + 1 = 0 sin dependencias
    }
    return levels;
}

} // namespace vx
