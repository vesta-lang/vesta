/**
 * @file namespace_flatten.cpp
 * @brief Pre-pass que aplana @c NamespaceDecl inline (Phase M.7.c).
 *
 * Transforma el AST tras parsear y antes del TypeChecker:
 *   1. Encuentra todos los @c NamespaceDecl top-level (y anidados).
 *   2. Para cada uno, aplica el prefijo de mangling a todas las
 *      declaraciones internas (FunctionDecl, ClassDecl, StructDecl,
 *      EnumDecl, TypeAliasDecl, GlobalVarDecl).  Anidamiento concatena:
 *      `namespace a { namespace b { class C } }` -> @c a__b__C.
 *   3. Reescribe REFERENCIAS internas (IdentExpr, NewExpr::class_name,
 *      NamedTypeNode, etc.) a los nombres mangled.
 *   4. "Sube" los decls extraidos al top-level del Module, descartando
 *      el @c NamespaceDecl wrapper.
 *   5. Devuelve un descriptor @c FlattenedNamespace por cada namespace
 *      encontrado, para que el caller los registre como
 *      @c Symbol::Namespace en el TypeChecker (reusa la infra de M7.a).
 *
 * Cero overhead runtime: el namespace es puramente lexical.  Los labels
 * mangled (`ui__Button`, `audio__Button`) son identificadores C validos,
 * compatibles con port-c y el linker actual del .velb.
 */

#include "vx/namespace_flatten.h"

#include <functional>
#include <unordered_map>

#include "vx/ast.h"

namespace vx {

namespace {

/// Phase NS.1: convierte un path de namespace PUNTEADO (`std.collections`) a su
/// forma MANGLED con separador `__` (`std__collections`).  Los nombres de un
/// solo segmento (M.7.c) quedan intactos.  Se usa SOLO al construir el prefijo
/// fisico de mangling; el nombre HUMANO (para resolucion / acceso qualified)
/// conserva los puntos.
std::string mangle_ns_path_(const std::string &dotted) {
    if (dotted.find('.') == std::string::npos) return dotted;
    std::string out;
    out.reserve(dotted.size() + 4);
    for (char c : dotted) {
        if (c == '.')
            out += "__";
        else
            out += c;
    }
    return out;
}

/// Quita el prefijo `<full_path>__` de un nombre mangled para obtener su nombre
/// publico local.  Si el nombre NO lleva ese prefijo (e.g. `main`, que no se
/// manglea, o un `__reservado`), lo devuelve TAL CUAL -- evita el substr
/// fuera-de-rango cuando un simbolo escapa al mangling (bug NS.1: la forma
/// statement `namespace a.b.c;` arrastra `main` al namespace).
std::string strip_ns_prefix_(const std::string &name,
                             const std::string &full_path) {
    if (full_path.empty()) return name;
    const std::string pre = full_path + "__";
    if (name.size() > pre.size() && name.compare(0, pre.size(), pre) == 0)
        return name.substr(pre.size());
    return name;
}

/// Aplica el prefix `<ns_path>__` a un nombre si NO empieza con `__`
/// (identificadores reservados) y no es `main` (entry point unico).
std::string mangle_name_(const std::string &ns_path, const std::string &name) {
    if (name.empty()) return name;
    if (name.size() >= 2 && name[0] == '_' && name[1] == '_') return name;
    if (name == "main") return name;
    return ns_path + "__" + name;
}

/// Walker recursivo que reescribe IdentExpr / NamedTypeNode / NewExpr
/// que matchean nombres del @p rename_map.  Mismo patron que
/// @c mangle_top_level_ de @c compiler_project.cpp pero re-uso aqui
/// para mantener namespace_flatten autocontenido (compile_project usa
/// este modulo para los deps con namespaces tambien).
void rewrite_refs_in_expr_(
    ast::Expr *e,
    const std::unordered_map<std::string, std::string> &rename_map);
void rewrite_refs_in_stmt_(
    ast::Stmt *s,
    const std::unordered_map<std::string, std::string> &rename_map);
void rewrite_refs_in_type_(
    ast::TypeNode *t,
    const std::unordered_map<std::string, std::string> &rename_map);

void rewrite_refs_in_type_(
    ast::TypeNode *t,
    const std::unordered_map<std::string, std::string> &rename_map) {
    if (!t) return;
    if (t->kind == ast::NodeKind::NamedTypeNode) {
        auto *nt = static_cast<ast::NamedTypeNode *>(t);
        auto it = rename_map.find(nt->name);
        if (it != rename_map.end()) nt->name = it->second;
        for (auto &ta : nt->type_args)
            rewrite_refs_in_type_(ta.get(), rename_map);
    } else if (t->kind == ast::NodeKind::PointerTypeNode) {
        auto *pt = static_cast<ast::PointerTypeNode *>(t);
        rewrite_refs_in_type_(pt->pointee.get(), rename_map);
    } else if (t->kind == ast::NodeKind::ArrayTypeNode) {
        auto *at = static_cast<ast::ArrayTypeNode *>(t);
        rewrite_refs_in_type_(at->element_type.get(), rename_map);
    } else if (t->kind == ast::NodeKind::FunctionTypeNode) {
        auto *ft = static_cast<ast::FunctionTypeNode *>(t);
        for (auto &pt : ft->param_types)
            rewrite_refs_in_type_(pt.get(), rename_map);
        rewrite_refs_in_type_(ft->return_type.get(), rename_map);
    } else if (t->kind == ast::NodeKind::PrimitiveTypeNode) {
        // NS.1 fix: los smart pointers (gc<T>/unique<T>/shared<T>/borrow<T>) y
        // las colecciones (ArrayList<T>/HashMap<K,V>/...) se parsean como
        // PrimitiveTypeNode con type_args -> hay que manglar los tipos internos.
        auto *pn = static_cast<ast::PrimitiveTypeNode *>(t);
        for (auto &ta : pn->type_args)
            rewrite_refs_in_type_(ta.get(), rename_map);
    }
}

void rewrite_refs_in_expr_(
    ast::Expr *e,
    const std::unordered_map<std::string, std::string> &rename_map) {
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
        rewrite_refs_in_expr_(c->callee.get(), rename_map);
        for (auto &a : c->args)
            rewrite_refs_in_expr_(a.get(), rename_map);
        for (auto &ta : c->type_args)
            rewrite_refs_in_type_(ta.get(), rename_map);
        // NS.1 fix: reflexion.  forName("ClassName") / Class.forName("ClassName")
        // referencian una clase por STRING; si la clase es del namespace esta
        // mangled -> reescribir el literal para que el lookup runtime la
        // encuentre.
        {
            const ast::IdentExpr *cid = nullptr;
            if (c->callee &&
                c->callee->kind == ast::NodeKind::IdentExpr)
                cid = static_cast<const ast::IdentExpr *>(c->callee.get());
            else if (c->callee &&
                     c->callee->kind == ast::NodeKind::FieldAccessExpr)
                // Class.forName(...) -> el field es "forName".
                cid = nullptr; // el field_name se chequea abajo
            const std::string fname =
                cid ? cid->name
                    : (c->callee &&
                       c->callee->kind == ast::NodeKind::FieldAccessExpr)
                          ? static_cast<const ast::FieldAccessExpr *>(
                                c->callee.get())
                                ->field_name
                          : std::string();
            // Builtins de reflexion que reciben un nombre de tipo/clase como
            // STRING literal: forName (clase), find_type (@Introspect).
            if (fname == "forName" || fname == "find_type") {
                for (auto &a : c->args) {
                    if (a && a->kind == ast::NodeKind::StringLitExpr) {
                        auto *sl = static_cast<ast::StringLitExpr *>(a.get());
                        if (sl->interp_exprs.empty()) {
                            auto it = rename_map.find(sl->value);
                            if (it != rename_map.end()) sl->value = it->second;
                        }
                    }
                }
            }
        }
        break;
    }
    case ast::NodeKind::NewExpr: {
        auto *ne = static_cast<ast::NewExpr *>(e);
        auto it = rename_map.find(ne->class_name);
        if (it != rename_map.end()) ne->class_name = it->second;
        for (auto &a : ne->args)
            rewrite_refs_in_expr_(a.get(), rename_map);
        for (auto &ta : ne->type_args)
            rewrite_refs_in_type_(ta.get(), rename_map);
        if (ne->array_size)
            rewrite_refs_in_expr_(ne->array_size.get(), rename_map);
        break;
    }
    case ast::NodeKind::BinaryExpr: {
        auto *b = static_cast<ast::BinaryExpr *>(e);
        rewrite_refs_in_expr_(b->lhs.get(), rename_map);
        rewrite_refs_in_expr_(b->rhs.get(), rename_map);
        break;
    }
    case ast::NodeKind::UnaryExpr: {
        auto *u = static_cast<ast::UnaryExpr *>(e);
        rewrite_refs_in_expr_(u->operand.get(), rename_map);
        break;
    }
    case ast::NodeKind::AssignExpr: {
        auto *a = static_cast<ast::AssignExpr *>(e);
        rewrite_refs_in_expr_(a->target.get(), rename_map);
        rewrite_refs_in_expr_(a->value.get(), rename_map);
        break;
    }
    case ast::NodeKind::IndexExpr: {
        auto *ix = static_cast<ast::IndexExpr *>(e);
        rewrite_refs_in_expr_(ix->base.get(), rename_map);
        rewrite_refs_in_expr_(ix->index.get(), rename_map);
        break;
    }
    case ast::NodeKind::FieldAccessExpr: {
        auto *fa = static_cast<ast::FieldAccessExpr *>(e);
        // Caso especial: si la base es IdentExpr cuyo nombre matchea
        // un enum renamed, reescribir.  Asi `Color.Red` -> `ns__Color.Red`.
        rewrite_refs_in_expr_(fa->base.get(), rename_map);
        break;
    }
    case ast::NodeKind::CastExpr: {
        auto *ce = static_cast<ast::CastExpr *>(e);
        rewrite_refs_in_expr_(ce->operand.get(), rename_map);
        rewrite_refs_in_type_(ce->target_type.get(), rename_map);
        break;
    }
    case ast::NodeKind::TernaryExpr: {
        auto *tn = static_cast<ast::TernaryExpr *>(e);
        rewrite_refs_in_expr_(tn->cond.get(), rename_map);
        rewrite_refs_in_expr_(tn->then_expr.get(), rename_map);
        rewrite_refs_in_expr_(tn->else_expr.get(), rename_map);
        break;
    }
    // NS.1 fix: nodos que faltaban en el walker -> referencias a simbolos del
    // namespace dentro de estos NO se re-manglaban (bug: `${fn()}`, lambdas,
    // match, spawn, init-lists, super(...), try-op).
    case ast::NodeKind::StringLitExpr: {
        auto *sl = static_cast<ast::StringLitExpr *>(e);
        for (auto &ie : sl->interp_exprs)
            rewrite_refs_in_expr_(ie.get(), rename_map);
        break;
    }
    case ast::NodeKind::TryExpr: {
        auto *te = static_cast<ast::TryExpr *>(e);
        rewrite_refs_in_expr_(te->operand.get(), rename_map);
        break;
    }
    case ast::NodeKind::SpawnExpr: {
        auto *sp = static_cast<ast::SpawnExpr *>(e);
        rewrite_refs_in_expr_(sp->sched_idx.get(), rename_map);
        rewrite_refs_in_stmt_(sp->body.get(), rename_map);
        break;
    }
    case ast::NodeKind::RSpawnExpr: {
        auto *rs = static_cast<ast::RSpawnExpr *>(e);
        rewrite_refs_in_expr_(rs->node_idx.get(), rename_map);
        rewrite_refs_in_stmt_(rs->body.get(), rename_map);
        break;
    }
    case ast::NodeKind::LambdaExpr: {
        auto *lm = static_cast<ast::LambdaExpr *>(e);
        for (auto &p : lm->params)
            rewrite_refs_in_type_(p->type.get(), rename_map);
        rewrite_refs_in_type_(lm->return_type.get(), rename_map);
        rewrite_refs_in_stmt_(lm->body.get(), rename_map);
        break;
    }
    case ast::NodeKind::MatchExpr: {
        auto *mt = static_cast<ast::MatchExpr *>(e);
        rewrite_refs_in_expr_(mt->scrutinee.get(), rename_map);
        for (auto &arm : mt->arms) {
            rewrite_refs_in_expr_(arm.guard.get(), rename_map);
            rewrite_refs_in_stmt_(arm.body.get(), rename_map);
        }
        break;
    }
    case ast::NodeKind::SuperCallExpr: {
        auto *sc = static_cast<ast::SuperCallExpr *>(e);
        for (auto &a : sc->args)
            rewrite_refs_in_expr_(a.get(), rename_map);
        break;
    }
    case ast::NodeKind::SuperMethodCallExpr: {
        auto *sm = static_cast<ast::SuperMethodCallExpr *>(e);
        for (auto &a : sm->args)
            rewrite_refs_in_expr_(a.get(), rename_map);
        break;
    }
    case ast::NodeKind::InitListExpr: {
        auto *il = static_cast<ast::InitListExpr *>(e);
        for (auto &el : il->elements)
            rewrite_refs_in_expr_(el.get(), rename_map);
        break;
    }
    default: break;
    }
}

void rewrite_refs_in_stmt_(
    ast::Stmt *s,
    const std::unordered_map<std::string, std::string> &rename_map) {
    if (!s) return;
    switch (s->kind) {
    case ast::NodeKind::BlockStmt: {
        auto *b = static_cast<ast::BlockStmt *>(s);
        for (auto &c : b->body)
            rewrite_refs_in_stmt_(c.get(), rename_map);
        break;
    }
    case ast::NodeKind::ExprStmt: {
        auto *es = static_cast<ast::ExprStmt *>(s);
        rewrite_refs_in_expr_(es->expr.get(), rename_map);
        break;
    }
    case ast::NodeKind::VarDeclStmt: {
        auto *vd = static_cast<ast::VarDeclStmt *>(s);
        rewrite_refs_in_type_(vd->type.get(), rename_map);
        if (vd->init) rewrite_refs_in_expr_(vd->init.get(), rename_map);
        break;
    }
    case ast::NodeKind::IfStmt: {
        auto *ifs = static_cast<ast::IfStmt *>(s);
        rewrite_refs_in_expr_(ifs->cond.get(), rename_map);
        rewrite_refs_in_stmt_(ifs->then_branch.get(), rename_map);
        rewrite_refs_in_stmt_(ifs->else_branch.get(), rename_map);
        break;
    }
    case ast::NodeKind::WhileStmt: {
        auto *w = static_cast<ast::WhileStmt *>(s);
        rewrite_refs_in_expr_(w->cond.get(), rename_map);
        rewrite_refs_in_stmt_(w->body.get(), rename_map);
        break;
    }
    case ast::NodeKind::ForStmt: {
        auto *fr = static_cast<ast::ForStmt *>(s);
        rewrite_refs_in_stmt_(fr->init.get(), rename_map);
        rewrite_refs_in_expr_(fr->cond.get(), rename_map);
        rewrite_refs_in_expr_(fr->step.get(), rename_map);
        rewrite_refs_in_stmt_(fr->body.get(), rename_map);
        break;
    }
    case ast::NodeKind::ReturnStmt: {
        auto *r = static_cast<ast::ReturnStmt *>(s);
        rewrite_refs_in_expr_(r->value.get(), rename_map);
        break;
    }
    // NS.1 fix: stmts que faltaban en el walker.
    case ast::NodeKind::DoWhileStmt: {
        auto *dw = static_cast<ast::DoWhileStmt *>(s);
        rewrite_refs_in_stmt_(dw->body.get(), rename_map);
        rewrite_refs_in_expr_(dw->cond.get(), rename_map);
        break;
    }
    case ast::NodeKind::ForEachStmt: {
        auto *fe = static_cast<ast::ForEachStmt *>(s);
        rewrite_refs_in_type_(fe->iter_type.get(), rename_map);
        rewrite_refs_in_expr_(fe->iter_expr.get(), rename_map);
        rewrite_refs_in_stmt_(fe->body.get(), rename_map);
        break;
    }
    case ast::NodeKind::ThrowStmt: {
        auto *th = static_cast<ast::ThrowStmt *>(s);
        rewrite_refs_in_expr_(th->value.get(), rename_map);
        break;
    }
    case ast::NodeKind::TryStmt: {
        auto *ts = static_cast<ast::TryStmt *>(s);
        rewrite_refs_in_stmt_(ts->body.get(), rename_map);
        for (auto &c : ts->catches) {
            auto it = rename_map.find(c.exc_class_name);
            if (it != rename_map.end()) c.exc_class_name = it->second;
            rewrite_refs_in_stmt_(c.body.get(), rename_map);
        }
        rewrite_refs_in_stmt_(ts->finally_body.get(), rename_map);
        break;
    }
    case ast::NodeKind::SynchronizedStmt: {
        auto *sy = static_cast<ast::SynchronizedStmt *>(s);
        rewrite_refs_in_expr_(sy->target.get(), rename_map);
        rewrite_refs_in_stmt_(sy->body.get(), rename_map);
        break;
    }
    case ast::NodeKind::ComptimeBlockStmt: {
        auto *cb = static_cast<ast::ComptimeBlockStmt *>(s);
        for (auto &st : cb->stmts)
            rewrite_refs_in_stmt_(st.get(), rename_map);
        break;
    }
    case ast::NodeKind::ComptimeForStmt: {
        auto *cf = static_cast<ast::ComptimeForStmt *>(s);
        rewrite_refs_in_expr_(cf->lo_expr.get(), rename_map);
        rewrite_refs_in_expr_(cf->hi_expr.get(), rename_map);
        rewrite_refs_in_stmt_(cf->body.get(), rename_map);
        break;
    }
    default: break;
    }
}

/// Mangle interno: aplica el prefix al nombre de cada decl + recorre
/// todos los hijos para aplicar los rewrites.  El @p rename_map se
/// EXTIENDE conforme el walker descubre nombres a renombrar.
void mangle_decls_(std::vector<std::unique_ptr<ast::Node>> &decls,
                   const std::string &ns_path,
                   std::unordered_map<std::string, std::string> &rename_map);

/// NS.1 fix: reescribe los nombres de CONCEPTS en los bounds de genericos
/// (`<T: MiConcepto>` / `where T: A + B`).  Sin esto, un concept declarado en el
/// namespace (mangled) no se resuelve en el bound ("concepto desconocido").
void rewrite_bounds_(
    std::vector<ast::TypeBound> &bounds,
    const std::unordered_map<std::string, std::string> &rename_map) {
    for (auto &b : bounds) {
        for (auto &c : b.concepts) {
            auto it = rename_map.find(c);
            if (it != rename_map.end()) c = it->second;
        }
    }
}

void mangle_function_decl_(
    ast::FunctionDecl *fd, const std::string &ns_path,
    std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, fd->name);
    if (newn != fd->name) {
        rename_map.emplace(fd->name, newn);
        fd->name = newn;
    }
    // Params + return: tipos pueden referenciar otros namespace-types.
    rewrite_refs_in_type_(fd->return_type.get(), rename_map);
    for (auto &p : fd->params) {
        // ParamDecl::type es un TypeNode.
        rewrite_refs_in_type_(p->type.get(), rename_map);
    }
    rewrite_bounds_(fd->type_bounds, rename_map);
    rewrite_refs_in_stmt_(fd->body.get(), rename_map);
}

void mangle_struct_decl_(
    ast::StructDecl *sd, const std::string &ns_path,
    std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, sd->name);
    if (newn != sd->name) {
        rename_map.emplace(sd->name, newn);
        sd->name = newn;
    }
    rewrite_bounds_(sd->type_bounds, rename_map);
    // Especializacion total/parcial: el patron `struct Caja<Punto>` guarda
    // los TypeNode del patron en spec_pattern.  Si un tipo del patron es del
    // namespace (e.g. Punto), debe manglearse igual que el arg de la
    // instanciacion, si no el match exacto (pt == arg) falla y cae al primario.
    for (auto &sp : sd->spec_pattern) {
        rewrite_refs_in_type_(sp.get(), rename_map);
    }
    for (auto &f : sd->fields) {
        rewrite_refs_in_type_(f.type.get(), rename_map);
    }
    // NS.1 fix: los STRUCTS tambien tienen metodos (dispatch estatico) + dtor.
    // Sus cuerpos deben reescribirse igual que los de clase, si no las refs a
    // globals/hermanos del namespace dentro de un metodo de struct fallan.
    for (auto &m : sd->methods) {
        if (!m) continue;
        rewrite_refs_in_type_(m->return_type.get(), rename_map);
        for (auto &p : m->params)
            rewrite_refs_in_type_(p->type.get(), rename_map);
        rewrite_bounds_(m->type_bounds, rename_map);
        rewrite_refs_in_stmt_(m->body.get(), rename_map);
    }
}

void mangle_class_decl_(
    ast::ClassDecl *cd, const std::string &ns_path,
    std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, cd->name);
    if (newn != cd->name) {
        rename_map.emplace(cd->name, newn);
        cd->name = newn;
    }
    // Super class y interfaces: si tambien estan en el rename_map, ajustar.
    {
        auto it = rename_map.find(cd->super_name);
        if (it != rename_map.end()) cd->super_name = it->second;
    }
    for (auto &iname : cd->interface_names) {
        auto it = rename_map.find(iname);
        if (it != rename_map.end()) iname = it->second;
    }
    // Especializacion total/parcial de clase (mismo motivo que en struct).
    for (auto &sp : cd->spec_pattern) {
        rewrite_refs_in_type_(sp.get(), rename_map);
    }
    // Fields + metodos.
    for (auto &f : cd->fields) {
        rewrite_refs_in_type_(f.type.get(), rename_map);
    }
    rewrite_bounds_(cd->type_bounds, rename_map);
    for (auto &m : cd->methods) {
        rewrite_refs_in_type_(m->return_type.get(), rename_map);
        for (auto &p : m->params)
            rewrite_refs_in_type_(p->type.get(), rename_map);
        rewrite_bounds_(m->type_bounds, rename_map);
        rewrite_refs_in_stmt_(m->body.get(), rename_map);
        // NS.1 fix: AOP.  El pointcut de un advice (@Before/@After/@Around) se
        // guarda como string "ClassName.methodName" en advice_target.  Si la
        // clase target es del namespace, esta mangled -> reescribir la parte de
        // la clase para que el pointcut matchee (si no, el advice no se registra
        // y el @After no pisa el resultado).
        if (!m->advice_target.empty()) {
            size_t dot = m->advice_target.find('.');
            if (dot != std::string::npos) {
                std::string cls = m->advice_target.substr(0, dot);
                auto it = rename_map.find(cls);
                if (it != rename_map.end())
                    m->advice_target =
                        it->second + m->advice_target.substr(dot);
            }
        }
    }
}

void mangle_enum_decl_(
    ast::EnumDecl *ed, const std::string &ns_path,
    std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, ed->name);
    if (newn != ed->name) {
        rename_map.emplace(ed->name, newn);
        ed->name = newn;
    }
    rewrite_bounds_(ed->type_bounds, rename_map);
    // Variantes: pueden tener payload types que apunten a otros
    // namespace-types.
    for (auto &v : ed->variants) {
        for (auto &pt : v.field_types) {
            rewrite_refs_in_type_(pt.get(), rename_map);
        }
    }
}

void mangle_typealias_decl_(
    ast::TypeAliasDecl *td, const std::string &ns_path,
    std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, td->name);
    if (newn != td->name) {
        rename_map.emplace(td->name, newn);
        td->name = newn;
    }
    rewrite_refs_in_type_(td->aliased.get(), rename_map);
}

void mangle_global_var_decl_(
    ast::GlobalVarDecl *gd, const std::string &ns_path,
    std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, gd->name);
    if (newn != gd->name) {
        rename_map.emplace(gd->name, newn);
        gd->name = newn;
    }
    rewrite_refs_in_type_(gd->type.get(), rename_map);
    if (gd->init) rewrite_refs_in_expr_(gd->init.get(), rename_map);
}

void mangle_concept_decl_(
    ast::ConceptDecl *cd, const std::string &ns_path,
    std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, cd->name);
    if (newn != cd->name) {
        rename_map.emplace(cd->name, newn);
        cd->name = newn;
    }
    // El predicado puede referenciar otros conceptos del mismo namespace
    // (composicion `A<T>() && B<T>()`); reescribir sus referencias.
    if (cd->predicate) rewrite_refs_in_expr_(cd->predicate.get(), rename_map);
}

void mangle_decls_(std::vector<std::unique_ptr<ast::Node>> &decls,
                   const std::string &ns_path,
                   std::unordered_map<std::string, std::string> &rename_map) {
    // Primera pasada: recolectar los nombres a renombrar (sin tocar bodies
    // todavia).  Asi cuando la segunda pasada reescribe referencias,
    // todos los nombres del namespace ya estan en el rename_map.
    for (auto &d : decls) {
        if (!d) continue;
        switch (d->kind) {
        case ast::NodeKind::FunctionDecl: {
            auto *fd = static_cast<ast::FunctionDecl *>(d.get());
            const std::string newn = mangle_name_(ns_path, fd->name);
            if (newn != fd->name) rename_map.emplace(fd->name, newn);
            break;
        }
        case ast::NodeKind::StructDecl: {
            auto *sd = static_cast<ast::StructDecl *>(d.get());
            const std::string newn = mangle_name_(ns_path, sd->name);
            if (newn != sd->name) rename_map.emplace(sd->name, newn);
            break;
        }
        case ast::NodeKind::ClassDecl: {
            auto *cd = static_cast<ast::ClassDecl *>(d.get());
            const std::string newn = mangle_name_(ns_path, cd->name);
            if (newn != cd->name) rename_map.emplace(cd->name, newn);
            break;
        }
        case ast::NodeKind::EnumDecl: {
            auto *ed = static_cast<ast::EnumDecl *>(d.get());
            const std::string newn = mangle_name_(ns_path, ed->name);
            if (newn != ed->name) rename_map.emplace(ed->name, newn);
            break;
        }
        case ast::NodeKind::TypeAliasDecl: {
            auto *td = static_cast<ast::TypeAliasDecl *>(d.get());
            const std::string newn = mangle_name_(ns_path, td->name);
            if (newn != td->name) rename_map.emplace(td->name, newn);
            break;
        }
        case ast::NodeKind::GlobalVarDecl: {
            auto *gd = static_cast<ast::GlobalVarDecl *>(d.get());
            const std::string newn = mangle_name_(ns_path, gd->name);
            if (newn != gd->name) rename_map.emplace(gd->name, newn);
            break;
        }
        case ast::NodeKind::ConceptDecl: {
            auto *cd = static_cast<ast::ConceptDecl *>(d.get());
            const std::string newn = mangle_name_(ns_path, cd->name);
            if (newn != cd->name) rename_map.emplace(cd->name, newn);
            break;
        }
        case ast::NodeKind::NamespaceDecl: {
            // Pre-recolectar los nombres del namespace anidado con
            // el prefix combinado.
            auto *nd = static_cast<ast::NamespaceDecl *>(d.get());
            const std::string nested_path =
                ns_path + "__" + mangle_ns_path_(nd->name);
            mangle_decls_(nd->decls, nested_path, rename_map);
            // El namespace decl mismo no se renombra; se procesa al
            // aplanar en collect_and_flatten_.
            break;
        }
        default: break;
        }
    }
    // Segunda pasada: aplicar el rename + reescribir referencias.
    for (auto &d : decls) {
        if (!d) continue;
        switch (d->kind) {
        case ast::NodeKind::FunctionDecl:
            mangle_function_decl_(static_cast<ast::FunctionDecl *>(d.get()),
                                  ns_path, rename_map);
            break;
        case ast::NodeKind::StructDecl:
            mangle_struct_decl_(static_cast<ast::StructDecl *>(d.get()),
                                ns_path, rename_map);
            break;
        case ast::NodeKind::ClassDecl:
            mangle_class_decl_(static_cast<ast::ClassDecl *>(d.get()), ns_path,
                               rename_map);
            break;
        case ast::NodeKind::EnumDecl:
            mangle_enum_decl_(static_cast<ast::EnumDecl *>(d.get()), ns_path,
                              rename_map);
            break;
        case ast::NodeKind::TypeAliasDecl:
            mangle_typealias_decl_(static_cast<ast::TypeAliasDecl *>(d.get()),
                                   ns_path, rename_map);
            break;
        case ast::NodeKind::GlobalVarDecl:
            mangle_global_var_decl_(static_cast<ast::GlobalVarDecl *>(d.get()),
                                    ns_path, rename_map);
            break;
        case ast::NodeKind::ConceptDecl:
            mangle_concept_decl_(static_cast<ast::ConceptDecl *>(d.get()),
                                 ns_path, rename_map);
            break;
        default: break;
        }
    }
}

/// Recolecta los nombres publicos (mangled) que cada namespace contiene
/// + sube los decls al @p out_decls al nivel del Module raiz.  Recursivo
/// para soportar namespaces anidados (las decls del nivel mas interno
/// se suben TODAS al mismo top-level).
void collect_and_flatten_(std::vector<std::unique_ptr<ast::Node>> &in_decls,
                          const std::string &local_ns_name,
                          const std::string &full_path,
                          FlattenedNamespace &out_ns,
                          std::vector<std::unique_ptr<ast::Node>> &out_decls) {
    for (auto &d : in_decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::NamespaceDecl) {
            // Anidado: el nombre del namespace del fichero original sigue
            // siendo el namespace LOCAL (e.g. "controls" dentro de "ui").
            // El path completo del prefix de mangling es ya `ui__controls`.
            auto *nd = static_cast<ast::NamespaceDecl *>(d.get());
            const std::string nested_path =
                full_path + "__" + mangle_ns_path_(nd->name);
            // El namespace anidado se trata como un namespace SEPARADO
            // accesible via `ui.controls.X` (anidamiento de simbolos).
            // En MVP solo soportamos un nivel (ui.X); los simbolos del
            // nivel anidado se agregan al namespace padre con sus nombres
            // mangled completos.
            FlattenedNamespace nested;
            nested.name =
                local_ns_name + "." + nd->name; // representacion humana
            collect_and_flatten_(nd->decls, nd->name, nested_path, nested,
                                 out_decls);
            // Volcar los simbolos del nested al padre con sus nombres
            // completos (`controls.Button` se accederia como
            // `ui.controls.Button`
            // -- la base sigue siendo el namespace raiz `ui`).  Para MVP
            // soportamos solo notacion `ui_controls_Button` (los hijos
            // se aplanan al padre con el prefix completo en su nombre publico).
            for (auto &sym : nested.symbols) {
                // El nombre publico que se accede como `ui.<X>` donde X
                // es `<nested_name>__<sym_name>`.  E.g. `controls__Button`.
                FlattenedNamespace::Sym child = sym;
                child.public_name =
                    mangle_ns_path_(nd->name) + "__" + sym.public_name;
                out_ns.symbols.push_back(std::move(child));
            }
            continue;
        }
        // Decls normales: extraer el nombre publico (mangled) + apilar.
        switch (d->kind) {
        case ast::NodeKind::FunctionDecl: {
            auto *fd = static_cast<ast::FunctionDecl *>(d.get());
            FlattenedNamespace::Sym sym;
            sym.kind = FlattenedNamespace::Sym::Function;
            sym.public_name =
                strip_ns_prefix_(fd->name, full_path);
            sym.mangled_label = fd->name;
            out_ns.symbols.push_back(std::move(sym));
            out_decls.push_back(std::move(d));
            break;
        }
        case ast::NodeKind::StructDecl: {
            auto *sd = static_cast<ast::StructDecl *>(d.get());
            FlattenedNamespace::Sym sym;
            sym.kind = FlattenedNamespace::Sym::Type;
            sym.public_name =
                strip_ns_prefix_(sd->name, full_path);
            sym.mangled_label = sd->name;
            out_ns.symbols.push_back(std::move(sym));
            out_decls.push_back(std::move(d));
            break;
        }
        case ast::NodeKind::ClassDecl: {
            auto *cd = static_cast<ast::ClassDecl *>(d.get());
            FlattenedNamespace::Sym sym;
            sym.kind = FlattenedNamespace::Sym::Type;
            sym.public_name =
                strip_ns_prefix_(cd->name, full_path);
            sym.mangled_label = cd->name;
            out_ns.symbols.push_back(std::move(sym));
            out_decls.push_back(std::move(d));
            break;
        }
        case ast::NodeKind::EnumDecl: {
            auto *ed = static_cast<ast::EnumDecl *>(d.get());
            FlattenedNamespace::Sym sym;
            sym.kind = FlattenedNamespace::Sym::Type;
            sym.public_name =
                strip_ns_prefix_(ed->name, full_path);
            sym.mangled_label = ed->name;
            out_ns.symbols.push_back(std::move(sym));
            out_decls.push_back(std::move(d));
            break;
        }
        case ast::NodeKind::TypeAliasDecl: {
            auto *td = static_cast<ast::TypeAliasDecl *>(d.get());
            FlattenedNamespace::Sym sym;
            sym.kind = FlattenedNamespace::Sym::Type;
            sym.public_name =
                strip_ns_prefix_(td->name, full_path);
            sym.mangled_label = td->name;
            out_ns.symbols.push_back(std::move(sym));
            out_decls.push_back(std::move(d));
            break;
        }
        case ast::NodeKind::GlobalVarDecl: {
            auto *gd = static_cast<ast::GlobalVarDecl *>(d.get());
            FlattenedNamespace::Sym sym;
            sym.kind = FlattenedNamespace::Sym::Variable;
            sym.public_name =
                strip_ns_prefix_(gd->name, full_path);
            sym.mangled_label = gd->name;
            out_ns.symbols.push_back(std::move(sym));
            out_decls.push_back(std::move(d));
            break;
        }
        case ast::NodeKind::ConceptDecl: {
            auto *cd = static_cast<ast::ConceptDecl *>(d.get());
            FlattenedNamespace::Sym sym;
            sym.kind = FlattenedNamespace::Sym::Type; // concepto = simbolo tipo-like
            sym.public_name = strip_ns_prefix_(cd->name, full_path);
            sym.mangled_label = cd->name;
            out_ns.symbols.push_back(std::move(sym));
            out_decls.push_back(std::move(d));
            break;
        }
        default:
            // Otros decls (ImportDecl, etc.) los pasamos sin modificar.
            out_decls.push_back(std::move(d));
            break;
        }
    }
}

} // namespace

std::vector<FlattenedNamespace> flatten_namespaces(ast::ModuleNode &mod) {
    std::vector<FlattenedNamespace> namespaces;
    // 1. Recorrer top-level: para cada NamespaceDecl, hacer mangling
    //    interno + extraer decls.
    std::vector<std::unique_ptr<ast::Node>> new_decls;
    new_decls.reserve(mod.decls.size() * 2);

    for (auto &d : mod.decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::NamespaceDecl) {
            auto *nd = static_cast<ast::NamespaceDecl *>(d.get());
            // Mangle todos los decls internos con el prefijo del namespace.
            // El prefijo FISICO usa la forma mangled (std.collections ->
            // std__collections); el nombre HUMANO (ns.name / local_ns_name)
            // conserva los puntos para la resolucion / acceso qualified.
            const std::string mangled_prefix = mangle_ns_path_(nd->name);
            std::unordered_map<std::string, std::string> rename_map;
            mangle_decls_(nd->decls, mangled_prefix, rename_map);
            // Recolectar simbolos publicos + subir decls al top-level.
            FlattenedNamespace ns;
            ns.name = nd->name; // humano (con puntos)
            collect_and_flatten_(nd->decls, nd->name, mangled_prefix, ns,
                                 new_decls);
            namespaces.push_back(std::move(ns));
            // El NamespaceDecl wrapper se descarta (no se añade a new_decls).
            // Sus decls internos ya estan en new_decls con nombres mangled.
        } else {
            new_decls.push_back(std::move(d));
        }
    }

    mod.decls = std::move(new_decls);
    return namespaces;
}

} // namespace vx
