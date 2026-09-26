/**
 * @file unit_prepare.cpp
 * @brief Preparar el AST de un modulo antes de comprobar sus tipos: el prefijo
 *        de modulo, el aplanado de namespaces y su conjunto comptime.
 */
#include "vx/unit/compile_unit.h"

#include "util/env_flags.h"
#include "vx/ast.h"
#include "vx/comptime/comptime_collect.h"
#include "vx/project/module_names.h" // module_symbol_prefix
#include "vx/project/module_paths.h" // module_dump_path
#include "vx/project/module_work.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <unordered_map>

namespace vx {

namespace {

/// Nombre original -> nombre con el prefijo del modulo.
using RenameMap = std::unordered_map<std::string, std::string>;

void rename_in_stmt(ast::Stmt *s, const RenameMap &rename_map);

/**
 * @brief Si @p c puede formar parte de un identificador NASM.
 * @param c El caracter.
 * @return @c true si es letra, digito o `_`.
 */
bool is_ident_ch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/**
 * @brief Reescribe los identificadores renombrados dentro de una expresion.
 * @param e          La expresion (puede ser nula).
 * @param rename_map Los renombrados del modulo.
 */
void rename_in_expr(ast::Expr *e, const RenameMap &rename_map) {
    if (!e) return;
    switch (e->kind) {
    case ast::NodeKind::IdentExpr: {
        auto *id = static_cast<ast::IdentExpr *>(e);
        auto it = rename_map.find(id->name);
        if (it != rename_map.end()) id->name = it->second;
        break;
    }
    case ast::NodeKind::CallExpr: {
        auto *c = static_cast<ast::CallExpr *>(e);
        rename_in_expr(c->callee.get(), rename_map);
        for (auto &a : c->args)
            rename_in_expr(a.get(), rename_map);
        break;
    }
    case ast::NodeKind::BinaryExpr: {
        auto *b = static_cast<ast::BinaryExpr *>(e);
        rename_in_expr(b->lhs.get(), rename_map);
        rename_in_expr(b->rhs.get(), rename_map);
        break;
    }
    case ast::NodeKind::UnaryExpr: {
        auto *u = static_cast<ast::UnaryExpr *>(e);
        rename_in_expr(u->operand.get(), rename_map);
        break;
    }
    case ast::NodeKind::AssignExpr: {
        auto *a = static_cast<ast::AssignExpr *>(e);
        rename_in_expr(a->target.get(), rename_map);
        rename_in_expr(a->value.get(), rename_map);
        break;
    }
    case ast::NodeKind::IndexExpr: {
        auto *ix = static_cast<ast::IndexExpr *>(e);
        rename_in_expr(ix->base.get(), rename_map);
        rename_in_expr(ix->index.get(), rename_map);
        break;
    }
    case ast::NodeKind::FieldAccessExpr: {
        auto *fa = static_cast<ast::FieldAccessExpr *>(e);
        rename_in_expr(fa->base.get(), rename_map);
        break;
    }
    case ast::NodeKind::CastExpr: {
        auto *ce = static_cast<ast::CastExpr *>(e);
        rename_in_expr(ce->operand.get(), rename_map);
        break;
    }
    case ast::NodeKind::TernaryExpr: {
        auto *tn = static_cast<ast::TernaryExpr *>(e);
        rename_in_expr(tn->cond.get(), rename_map);
        rename_in_expr(tn->then_expr.get(), rename_map);
        rename_in_expr(tn->else_expr.get(), rename_map);
        break;
    }
    case ast::NodeKind::NewExpr: {
        auto *ne = static_cast<ast::NewExpr *>(e);
        for (auto &a : ne->args)
            rename_in_expr(a.get(), rename_map);
        break;
    }
    case ast::NodeKind::StringLitExpr: {
        // Interpolaciones `"...${expr}..."`: recorrer cada expr
        // interna.  Sin esto, identificadores referenciados desde
        // dentro de un string interpolado no se manglan cuando el
        // modulo se compila como dep.
        auto *sl = static_cast<ast::StringLitExpr *>(e);
        for (auto &ie : sl->interp_exprs)
            rename_in_expr(ie.get(), rename_map);
        break;
    }
    case ast::NodeKind::LambdaExpr: {
        auto *la = static_cast<ast::LambdaExpr *>(e);
        if (la->body) rename_in_stmt(la->body.get(), rename_map);
        break;
    }
    case ast::NodeKind::MatchExpr: {
        auto *me = static_cast<ast::MatchExpr *>(e);
        rename_in_expr(me->scrutinee.get(), rename_map);
        for (auto &arm : me->arms) {
            if (arm.guard) rename_in_expr(arm.guard.get(), rename_map);
            if (arm.body) rename_in_stmt(arm.body.get(), rename_map);
        }
        break;
    }
    // Otros expr-kinds que pueden contener idents se cubren
    // conforme aparezcan en tests.
    default: break;
    }
}

/**
 * @brief Reescribe por TOKEN completo los simbolos renombrados de un cuerpo
 *        de asm.
 * @param body       El texto del bloque; se reescribe en su sitio.
 * @param rename_map Los renombrados del modulo.
 */
void rename_in_asm_body(std::string &body, const RenameMap &rename_map) {
    std::string out;
    out.reserve(body.size());
    size_t i = 0;
    while (i < body.size()) {
        char c = body[i];
        // Inicio de un identificador NASM (letra o `_`; el `.` de una
        // label local NO inicia identificador renombrable porque los
        // nombres de rename_map nunca empiezan por `.`).
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            size_t j = i + 1;
            while (j < body.size() && is_ident_ch(body[j]))
                ++j;
            std::string tok = body.substr(i, j - i);
            auto it = rename_map.find(tok);
            if (it != rename_map.end())
                out += it->second;
            else
                out += tok;
            i = j;
        } else {
            out += c;
            ++i;
        }
    }
    body.swap(out);
}

/**
 * @brief Reescribe los identificadores renombrados dentro de una sentencia.
 * @param s          La sentencia (puede ser nula).
 * @param rename_map Los renombrados del modulo.
 */
void rename_in_stmt(ast::Stmt *s, const RenameMap &rename_map) {
    if (!s) return;
    switch (s->kind) {
    case ast::NodeKind::BlockStmt: {
        auto *b = static_cast<ast::BlockStmt *>(s);
        for (auto &c : b->body)
            rename_in_stmt(c.get(), rename_map);
        break;
    }
    case ast::NodeKind::ExprStmt: {
        auto *es = static_cast<ast::ExprStmt *>(s);
        rename_in_expr(es->expr.get(), rename_map);
        break;
    }
    case ast::NodeKind::VarDeclStmt: {
        auto *vd = static_cast<ast::VarDeclStmt *>(s);
        if (vd->init) rename_in_expr(vd->init.get(), rename_map);
        break;
    }
    case ast::NodeKind::IfStmt: {
        auto *ifs = static_cast<ast::IfStmt *>(s);
        rename_in_expr(ifs->cond.get(), rename_map);
        rename_in_stmt(ifs->then_branch.get(), rename_map);
        rename_in_stmt(ifs->else_branch.get(), rename_map);
        break;
    }
    case ast::NodeKind::WhileStmt: {
        auto *w = static_cast<ast::WhileStmt *>(s);
        rename_in_expr(w->cond.get(), rename_map);
        rename_in_stmt(w->body.get(), rename_map);
        break;
    }
    case ast::NodeKind::ForStmt: {
        auto *fr = static_cast<ast::ForStmt *>(s);
        rename_in_stmt(fr->init.get(), rename_map);
        rename_in_expr(fr->cond.get(), rename_map);
        rename_in_expr(fr->step.get(), rename_map);
        rename_in_stmt(fr->body.get(), rename_map);
        break;
    }
    case ast::NodeKind::ReturnStmt: {
        auto *r = static_cast<ast::ReturnStmt *>(s);
        rename_in_expr(r->value.get(), rename_map);
        break;
    }
    case ast::NodeKind::AsmStmt: {
        // Inline-asm @Naked: el cuerpo es texto NASM verbatim.  Un
        // `call helper2` / `jmp helper2` / `lea rax, [helper2]` que
        // referencie una fn/global top-level del PROPIO modulo debe ver
        // el nombre MANGLED (`mod__helper2`), igual que un IdentExpr.
        // Sin esto, al compilar el modulo como dep el simbolo del asm
        // queda con su nombre LOCAL y el resolver de @Naked cross-modulo
        // (jit/naked_native.cpp::resolve_naked_symbol) no lo encuentra
        // -> "simbolo externo no resuelto" -> la fn @Naked no compila.
        // (Las labels internas del bloque `.foo:` empiezan por `.` y las
        // fns reservadas por `__`; ninguna esta en rename_map, no se
        // tocan.)  Reescritura por TOKEN completo (frontera de
        // identificador) para no pisar substrings de otro simbolo.
        auto *as = static_cast<ast::AsmStmt *>(s);
        rename_in_asm_body(as->body, rename_map);
        break;
    }
    default: break;
    }
}

///  M.5: renombrar las top-level FunctionDecl y GlobalVarDecl del
/// modulo con un prefijo `<modname>__`.  Esto evita colisiones de
/// nombres cuando dos modulos definen una funcion con el mismo nombre
/// (e.g. ambos `lib_a` y `lib_b` declaran `i32 init();`).
///
/// IMPORTANTE: las llamadas DENTRO del modulo a esas mismas funciones
/// tambien necesitan referenciar el nombre mangled.  Como el lowering
/// resuelve el nombre via @c IdentExpr::name, una segunda pasada
/// recorre las CALL exprs y reescribe sus nombres si encajan con un
/// simbolo renombrado del propio modulo.
///
/// NO renombramos:
///   - Identifiers que comienzan con `__` (estan reservados, p. ej.
///     `__module_init`, `__new_<X>`, `__macro_<X>` que el lowering
///     genera automaticamente).
///   - `main` (entry point unico del programa: solo el root lo define
///     y se invoca via su nombre canonico).
///
/// @param mod         El AST del modulo; se renombra en su sitio.
/// @param module_name El nombre del modulo, del que sale el prefijo.
void mangle_top_level_(ast::ModuleNode &mod, const std::string &module_name) {
    const std::string prefix = module_symbol_prefix(module_name);

    // Recopilar los nombres a renombrar.
    RenameMap rename_map;
    for (auto &decl : mod.decls) {
        if (!decl) continue;
        if (decl->kind == ast::NodeKind::FunctionDecl) {
            auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
            if (fd->name.empty()) continue;
            if (fd->name == "main") continue;
            if (fd->name.size() >= 2 && fd->name[0] == '_' &&
                fd->name[1] == '_')
                continue;
            // Mangle: lib::foo -> lib__foo.
            const std::string newn = prefix + fd->name;
            rename_map.emplace(fd->name, newn);
            fd->name = newn;
        } else if (decl->kind == ast::NodeKind::GlobalVarDecl) {
            auto *gd = static_cast<ast::GlobalVarDecl *>(decl.get());
            if (gd->name.empty()) continue;
            if (gd->name.size() >= 2 && gd->name[0] == '_' &&
                gd->name[1] == '_')
                continue;
            const std::string newn = prefix + gd->name;
            rename_map.emplace(gd->name, newn);
            gd->name = newn;
        }
    }
    if (rename_map.empty()) return;

    // Aplicar el walker a los bodies de cada FunctionDecl + GlobalVarDecl init,
    // mas los bodies de TODOS los metodos de cada ClassDecl (Bug Fix M.cm).
    // Sin esto, los metodos de clase no ven los simbolos mangled del
    // propio modulo cuando se compila como dep.
    for (auto &decl : mod.decls) {
        if (!decl) continue;
        if (decl->kind == ast::NodeKind::FunctionDecl) {
            auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
            rename_in_stmt(fd->body.get(), rename_map);
        } else if (decl->kind == ast::NodeKind::GlobalVarDecl) {
            auto *gd = static_cast<ast::GlobalVarDecl *>(decl.get());
            rename_in_expr(gd->init.get(), rename_map);
        } else if (decl->kind == ast::NodeKind::ClassDecl) {
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            for (auto &m : cd->methods) {
                if (m && m->body) rename_in_stmt(m->body.get(), rename_map);
            }
        }
    }
}

/**
 * @brief Recoge el conjunto comptime del modulo, con los nombres ya mangled.
 * @param pm                El modulo.
 * @param inline_namespaces Sus namespaces ya aplanados.
 */
void collect_unit_comptime(ProjectModuleWork &pm,
                           const std::vector<FlattenedNamespace> &inline_namespaces) {
    const ComptimeUnit cu = collect_comptime_unit(*pm.ast, pm.source);
    if (!cu.empty()) {
        /* Los conjuntos de TODOS los modulos se CONCATENAN en uno solo:
         * el trabajo comptime esta concentrado (medido: 6 de las 8
         * raices de este proyecto viven en un unico modulo), asi que
         * repartirlo en artefactos por modulo no tendria de que morder,
         * y ademas cada compilacion paga un suelo fijo (~8.5 ms) que se
         * multiplicaria por el numero de artefactos. */
        /* Con su `namespace` delante.  El conjunto de cada modulo
         * se concatena con los demas en UN solo texto, asi que sin el
         * todos caen en el mismo espacio de nombres y chocan entre si
         * (`nullptr`, `ANCHO`, `UMBRAL_*`...).  No sale de las decls
         * porque a estas alturas los namespaces ya estan APLANADOS: el
         * nombre lo tiene el aplanado, que es quien lo sabe. */
        /* En el MODULO, no en el acumulado del proyecto: esta lambda
         * corre en hasta ocho hilos a la vez y `res` es uno solo.  Un
         * `+=` sobre la misma `std::string` desde dos hilos revienta su
         * bufer -- el compilador moria con 0xC0000374 sin imprimir
         * nada, con victima distinta cada vuelta.  Cada modulo llena lo
         * suyo y la suma se hace despues del bucle, en orden de indice,
         * que ademas la vuelve determinista.  Con el `namespace`
         * delante: el texto tiene que sostenerse solo venga de donde
         * venga. */
        for (const auto &fns : inline_namespaces) {
            if (fns.name.empty()) continue;
            pm.comptime_unit_source += "namespace ";
            pm.comptime_unit_source += fns.name;
            pm.comptime_unit_source += ";";
            pm.comptime_unit_source.push_back(static_cast<char>(10));
            break;
        }
        pm.comptime_unit_source += cu.unit_source;
        pm.comptime_unit_hash = cu.content_hash;
        for (const auto *list : {&cu.comptime_fns, &cu.macros, &cu.helper_deps})
            for (const std::string &n : *list)
                pm.comptime_unit_names.push_back(util::InternedName::intern(n));
        pm.comptime_unit_not_collected.insert(
            pm.comptime_unit_not_collected.end(), cu.not_collected.begin(),
            cu.not_collected.end());
        if (util::flag_on(util::FlagId::DumpComptimeUnit)) {
            std::cerr << "[comptime-unit] modulo " << pm.module_name.str()
                      << "\n";
            dump_comptime_unit(cu, std::cerr);
        }
        /* El TEXTO extraido, a un fichero por modulo.  Cuando la suma
         * no compila hay que poder LEER lo que se extrajo: reproducirlo
         * con fuentes de juguete no lo consigue -- se intento con
         * `namespace` y con la concatenacion de dos modulos, y los dos
         * compilan bien. */
        if (!util::flag_text(util::FlagId::VolcarUnidad).empty()) {
            const std::string &d = util::flag_text(util::FlagId::VolcarUnidad);
            std::error_code vec;
            std::filesystem::create_directories(d, vec);
            /* Con la HUELLA de su ruta en el nombre: hay dos
             * `x86_64.vx` -- el de tipos y el de memoria --, y sin ella
             * uno pisaba al otro.  Ver `module_dump_path`. */
            std::ofstream f(module_dump_path(d, std::string(),
                                             pm.module_name.str(),
                                             pm.canonical_path.str(),
                                             ".unidad.vx"));
            if (f) f << cu.unit_source;
        }
    } else if (!cu.not_collected.empty()) {
        /* Conjunto VACIO pero con comptime dentro de un tipo.  Sin esta
         * rama, este caso y "el modulo no tiene nada comptime" se leen
         * IGUAL desde fuera, y son opuestos: aqui el artefacto se
         * quedaria sin algo que hace falta.  No saber es un resultado y
         * se dice por que. */
        pm.comptime_unit_not_collected.insert(
            pm.comptime_unit_not_collected.end(), cu.not_collected.begin(),
            cu.not_collected.end());
        if (util::flag_on(util::FlagId::DumpComptimeUnit)) {
            std::cerr << "[comptime-unit] modulo " << pm.module_name.str()
                      << "\n";
            dump_comptime_unit(cu, std::cerr);
        }
    }
}

} // namespace

std::vector<FlattenedNamespace> prepare_unit(const UnitEnv &env, size_t i) {
    ProjectModuleWork &pm = (*env.work)[i];
    const bool is_root = (i + 1 == env.work->size());

    // ---- Compile path (cache miss o root) ----
    //  M.5: si es DEP (no root), mangle top-level fns y globals
    // con prefijo `<module>__` para evitar colisiones cross-module.
    // El root NO se mangla (mantiene `main` y demas nombres tal cual).
    if (!is_root) {
        mangle_top_level_(*pm.ast, pm.module_name.str());
    }

    // NS.2: aplanar los `namespace X;` inline de ESTE modulo (root o dep).
    // mangle_top_level_ (arriba) solo toca las decls anonimas top-level (no
    // recorre NamespaceDecl), asi que no hay doble-mangle: las decls
    // namespaced se manglan por su NAMESPACE (mylib__X), las anonimas de un
    // dep por su MODULO (lib__Y).  Se registran en el TypeChecker para que
    // el acceso qualified (mylib.helper()) resuelva dentro del modulo.
    auto inline_namespaces = flatten_namespaces(*pm.ast);

    /* El conjunto comptime de ESTE modulo, con los nombres ya mangled.  El
     * volcado equivalente del camino de fichero suelto
     * (`compile_vx_source`) no sirve aqui: los modulos que de verdad tienen
     * comptime -- la stdlib
     * -- llegan por el camino de PROYECTO, asi que sin esto la medida se
     * tomaba sobre casos sinteticos y la granularidad del artefacto se
     * elegia por arquitectura en vez de por dato.  Solo diagnostico. */
    collect_unit_comptime(pm, inline_namespaces);
    return inline_namespaces;
}

} // namespace vx
