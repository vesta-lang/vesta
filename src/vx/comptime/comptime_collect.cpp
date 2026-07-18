/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file comptime_collect.cpp
 * @brief Implementacion del recolector del conjunto comptime (P1 fase 1).
 *
 * Analisis puro: recorre el modulo, clasifica las decls comptime y arrastra
 * (BFS) las funciones no-comptime que ese codigo llama, para que el futuro
 * artefacto comptime separado sea auto-suficiente.
 */

#include "vx/comptime/comptime_collect.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace vx {

namespace {

/// Acumula en @p out los nombres de funcion invocados (callee IdentExpr) dentro
/// de una expresion, recursivamente.  Solo nos interesan las llamadas directas
/// por nombre; las indirectas (punteros a fn) no arrastran una decl concreta.
void collect_calls_expr(const ast::Expr *e, std::unordered_set<std::string> &out) {
    if (!e) return;
    switch (e->kind) {
    case ast::NodeKind::CallExpr: {
        const auto *c = static_cast<const ast::CallExpr *>(e);
        if (c->callee && c->callee->kind == ast::NodeKind::IdentExpr) {
            out.insert(
                static_cast<const ast::IdentExpr *>(c->callee.get())->name);
        }
        collect_calls_expr(c->callee.get(), out);
        for (const auto &a : c->args)
            collect_calls_expr(a.get(), out);
        break;
    }
    case ast::NodeKind::BinaryExpr: {
        const auto *b = static_cast<const ast::BinaryExpr *>(e);
        collect_calls_expr(b->lhs.get(), out);
        collect_calls_expr(b->rhs.get(), out);
        break;
    }
    case ast::NodeKind::UnaryExpr:
        collect_calls_expr(static_cast<const ast::UnaryExpr *>(e)->operand.get(),
                           out);
        break;
    case ast::NodeKind::TernaryExpr: {
        const auto *t = static_cast<const ast::TernaryExpr *>(e);
        collect_calls_expr(t->cond.get(), out);
        collect_calls_expr(t->then_expr.get(), out);
        collect_calls_expr(t->else_expr.get(), out);
        break;
    }
    case ast::NodeKind::FieldAccessExpr:
        collect_calls_expr(
            static_cast<const ast::FieldAccessExpr *>(e)->base.get(), out);
        break;
    case ast::NodeKind::IndexExpr: {
        const auto *i = static_cast<const ast::IndexExpr *>(e);
        collect_calls_expr(i->base.get(), out);
        collect_calls_expr(i->index.get(), out);
        break;
    }
    default:
        break;
    }
}

/// Idem sobre statements (recorre las expresiones contenidas + control de
/// flujo).  Cubre los nodos que aparecen en cuerpos comptime tipicos.
void collect_calls_stmt(const ast::Stmt *s,
                        std::unordered_set<std::string> &out) {
    if (!s) return;
    switch (s->kind) {
    case ast::NodeKind::BlockStmt:
    case ast::NodeKind::ComptimeBlockStmt: {
        const auto *b = static_cast<const ast::BlockStmt *>(s);
        for (const auto &st : b->body)
            collect_calls_stmt(st.get(), out);
        break;
    }
    case ast::NodeKind::ExprStmt:
        collect_calls_expr(static_cast<const ast::ExprStmt *>(s)->expr.get(),
                           out);
        break;
    case ast::NodeKind::VarDeclStmt:
        collect_calls_expr(static_cast<const ast::VarDeclStmt *>(s)->init.get(),
                           out);
        break;
    case ast::NodeKind::ReturnStmt:
        collect_calls_expr(static_cast<const ast::ReturnStmt *>(s)->value.get(),
                           out);
        break;
    case ast::NodeKind::IfStmt: {
        const auto *i = static_cast<const ast::IfStmt *>(s);
        collect_calls_expr(i->cond.get(), out);
        collect_calls_stmt(i->then_branch.get(), out);
        collect_calls_stmt(i->else_branch.get(), out);
        break;
    }
    case ast::NodeKind::WhileStmt: {
        const auto *w = static_cast<const ast::WhileStmt *>(s);
        collect_calls_expr(w->cond.get(), out);
        collect_calls_stmt(w->body.get(), out);
        break;
    }
    case ast::NodeKind::DoWhileStmt: {
        const auto *w = static_cast<const ast::DoWhileStmt *>(s);
        collect_calls_expr(w->cond.get(), out);
        collect_calls_stmt(w->body.get(), out);
        break;
    }
    case ast::NodeKind::ForStmt: {
        const auto *f = static_cast<const ast::ForStmt *>(s);
        collect_calls_stmt(f->init.get(), out);
        collect_calls_expr(f->cond.get(), out);
        collect_calls_expr(f->step.get(), out);
        collect_calls_stmt(f->body.get(), out);
        break;
    }
    case ast::NodeKind::ComptimeForStmt: {
        const auto *f = static_cast<const ast::ComptimeForStmt *>(s);
        collect_calls_stmt(f->body.get(), out);
        break;
    }
    default:
        break;
    }
}

} // namespace

ComptimeUnit collect_comptime_unit(const ast::ModuleNode &mod,
                                   const std::string &source) {
    ComptimeUnit u;

    // Indice nombre -> FunctionDecl para resolver dependencias.
    std::unordered_map<std::string, const ast::FunctionDecl *> fn_by_name;
    for (const auto &d : mod.decls) {
        if (d && d->kind == ast::NodeKind::FunctionDecl) {
            const auto *fd = static_cast<const ast::FunctionDecl *>(d.get());
            fn_by_name.emplace(fd->name, fd);
        }
    }

    // Clasificar decls comptime top-level + sembrar el worklist de cuerpos.
    std::unordered_set<std::string> seed_calls; // llamadas del codigo comptime
    for (const auto &d : mod.decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::FunctionDecl) {
            const auto *fd = static_cast<const ast::FunctionDecl *>(d.get());
            if (fd->is_macro) {
                u.macros.push_back(fd->name);
                collect_calls_stmt(fd->body.get(), seed_calls);
            } else if (fd->is_comptime) {
                u.comptime_fns.push_back(fd->name);
                collect_calls_stmt(fd->body.get(), seed_calls);
            }
        } else if (d->kind == ast::NodeKind::GlobalVarDecl) {
            const auto *gv = static_cast<const ast::GlobalVarDecl *>(d.get());
            if (gv->is_comptime || gv->is_const) {
                u.comptime_consts.push_back(gv->name);
                collect_calls_expr(gv->init.get(), seed_calls);
            }
        } else if (d->kind == ast::NodeKind::ComptimeBlockStmt) {
            // Bloque comptime a nivel modulo: su cuerpo tambien es comptime.
            collect_calls_stmt(static_cast<const ast::Stmt *>(d.get()),
                               seed_calls);
        }
    }

    // Cierre transitivo: cada llamada a una fn NO-comptime del modulo se
    // arrastra como dependencia (debe viajar en el artefacto) y se explora su
    // cuerpo tambien.  Las comptime fn/macro ya estan en sus listas.
    std::unordered_set<std::string> is_comptime_name;
    for (const auto &n : u.comptime_fns) is_comptime_name.insert(n);
    for (const auto &n : u.macros) is_comptime_name.insert(n);

    std::unordered_set<std::string> visited;
    std::vector<std::string> work(seed_calls.begin(), seed_calls.end());
    std::unordered_set<std::string> dep_set;
    while (!work.empty()) {
        const std::string name = work.back();
        work.pop_back();
        if (!visited.insert(name).second) continue;
        auto it = fn_by_name.find(name);
        if (it == fn_by_name.end()) continue; // builtin/extern/no del modulo.
        // Si es comptime, ya esta en su lista; si no, es helper-dep.
        if (!is_comptime_name.count(name)) dep_set.insert(name);
        std::unordered_set<std::string> more;
        collect_calls_stmt(it->second->body.get(), more);
        for (const auto &m : more)
            if (!visited.count(m)) work.push_back(m);
    }
    u.helper_deps.assign(dep_set.begin(), dep_set.end());

    // Orden estable para diagnostico reproducible.
    std::sort(u.comptime_fns.begin(), u.comptime_fns.end());
    std::sort(u.macros.begin(), u.macros.end());
    std::sort(u.comptime_consts.begin(), u.comptime_consts.end());
    std::sort(u.helper_deps.begin(), u.helper_deps.end());

    // Clave de cache del artefacto: FNV-1a 64 del texto fuente de las decls del
    // conjunto (spans [decl.offset, siguiente_decl.offset)).  Reproducible e
    // independiente del codigo no-comptime del modulo.
    if (!source.empty() && !u.empty()) {
        std::unordered_set<std::string> unit_names;
        for (const auto &n : u.comptime_fns) unit_names.insert(n);
        for (const auto &n : u.macros) unit_names.insert(n);
        for (const auto &n : u.comptime_consts) unit_names.insert(n);
        for (const auto &n : u.helper_deps) unit_names.insert(n);

        struct Span {
            uint32_t off;
            bool in_unit;
        };
        std::vector<Span> spans;
        spans.reserve(mod.decls.size());
        for (const auto &d : mod.decls) {
            if (!d) continue;
            bool in = false;
            if (d->kind == ast::NodeKind::FunctionDecl)
                in = unit_names.count(
                         static_cast<const ast::FunctionDecl *>(d.get())->name) >
                     0;
            else if (d->kind == ast::NodeKind::GlobalVarDecl)
                in = unit_names.count(
                         static_cast<const ast::GlobalVarDecl *>(d.get())
                             ->name) > 0;
            else if (d->kind == ast::NodeKind::ComptimeBlockStmt)
                in = true; // el bloque comptime es parte del conjunto.
            spans.push_back({d->loc.offset, in});
        }
        std::sort(spans.begin(), spans.end(),
                  [](const Span &a, const Span &b) { return a.off < b.off; });

        uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis.
        const uint32_t src_len = static_cast<uint32_t>(source.size());
        for (size_t i = 0; i < spans.size(); ++i) {
            if (!spans[i].in_unit) continue;
            uint32_t start = spans[i].off;
            uint32_t end = (i + 1 < spans.size()) ? spans[i + 1].off : src_len;
            if (start > src_len) start = src_len;
            if (end > src_len) end = src_len;
            for (uint32_t j = start; j < end; ++j) {
                h ^= static_cast<uint8_t>(source[j]);
                h *= 1099511628211ULL; // FNV-1a prime.
            }
        }
        u.content_hash = h;
    }
    return u;
}

void dump_comptime_unit(const ComptimeUnit &u, std::ostream &os) {
    os << "[comptime-unit] resumen del conjunto comptime del modulo:\n";
    os << "  comptime fns   (" << u.comptime_fns.size() << "):";
    for (const auto &n : u.comptime_fns) os << " " << n;
    os << "\n  @Macro         (" << u.macros.size() << "):";
    for (const auto &n : u.macros) os << " " << n;
    os << "\n  comptime const (" << u.comptime_consts.size() << "):";
    for (const auto &n : u.comptime_consts) os << " " << n;
    os << "\n  helper deps    (" << u.helper_deps.size() << "):";
    for (const auto &n : u.helper_deps) os << " " << n;
    os << "\n  content_hash   : 0x" << std::hex << u.content_hash << std::dec
       << "  (clave de cache del artefacto comptime)\n";
}

} // namespace vx
