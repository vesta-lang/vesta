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

/**
 * @brief Principio REAL de la declaracion que empieza (segun el AST) en @p off.
 *
 * `loc.offset` de una decl apunta a su TIPO DE RETORNO, no a donde empieza de
 * verdad: lo que va delante -- `comptime`, `public`, anotaciones `@Macro` /
 * `@Pure`, el comentario de documentacion -- queda fuera.  Recortar por ahi
 * rompe el texto por los DOS lados: la decl extraida pierde su modificador (un
 * `comptime string f()` sale como `string f()`) y la decl ANTERIOR se queda con
 * ese `comptime` colgando al final, que es codigo invalido.  Se vio como "se
 * esperaba ';' al final de la declaracion" y "se esperaba un tipo al inicio de
 * la declaracion top-level" al compilar el conjunto extraido.
 *
 * Se retrocede hasta el principio de la linea de @p off y, desde ahi, por las
 * lineas anteriores mientras sean parte de la declaracion: anotaciones (`@`),
 * documentacion (`//`, `/*`, `*`) o una linea con solo modificadores.  Se para
 * en cuanto encuentra algo que ya es otra cosa (el `}` de la decl anterior, una
 * linea en blanco no precedida de anotacion, etc.).
 *
 * @param src Fuente completo del modulo.
 * @param off Offset que da el AST para la decl.
 * @return Offset del primer caracter de la declaracion, <= @p off.
 */
uint32_t inicio_real_de_decl(const std::string &src, uint32_t off) {
    if (off > src.size()) return static_cast<uint32_t>(src.size());
    // Principio de la linea de `off`.
    uint32_t ini = off;
    while (ini > 0 && src[ini - 1] != '\n')
        --ini;

    /// @brief Contenido de la linea que TERMINA en @p fin (exclusivo), sin
    ///        espacios de los extremos.
    auto linea_previa = [&src](uint32_t fin, uint32_t &ini_out) -> std::string {
        if (fin == 0) return {};
        uint32_t f = fin - 1; // saltar el '\n' que la cierra
        uint32_t i = f;
        while (i > 0 && src[i - 1] != '\n')
            --i;
        ini_out = i;
        std::string s = src.substr(i, f - i);
        size_t a = s.find_first_not_of(" \t\r");
        if (a == std::string::npos) return {};
        size_t b = s.find_last_not_of(" \t\r");
        return s.substr(a, b - a + 1);
    };

    for (;;) {
        uint32_t ini_prev = 0;
        const std::string prev = linea_previa(ini, ini_prev);
        if (prev.empty()) break; // linea en blanco o principio del fichero
        const bool es_anotacion = prev[0] == '@';
        const bool es_doc = prev.rfind("//", 0) == 0 ||
                            prev.rfind("/*", 0) == 0 || prev[0] == '*';
        /* Una linea que es SOLO modificadores (`public`, `comptime`, ...) sin
         * nada mas: es la cabecera de esta decl partida en dos lineas. */
        const bool es_modificador =
            prev == "comptime" || prev == "public" || prev == "private" ||
            prev == "protected" || prev == "static" || prev == "final" ||
            prev == "extern" || prev == "public comptime" ||
            prev == "private comptime";
        if (!es_anotacion && !es_doc && !es_modificador) break;
        ini = ini_prev;
    }
    return ini;
}

/// Acumula en @p out los nombres de funcion invocados (callee IdentExpr) dentro
/// de una expresion, recursivamente.  Solo nos interesan las llamadas directas
/// por nombre; las indirectas (punteros a fn) no arrastran una decl concreta.
void collect_calls_expr(const ast::Expr *e,
                        std::unordered_set<std::string> &out) {
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
        collect_calls_expr(
            static_cast<const ast::UnaryExpr *>(e)->operand.get(), out);
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
    default: break;
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
    default: break;
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
    for (const auto &n : u.comptime_fns)
        is_comptime_name.insert(n);
    for (const auto &n : u.macros)
        is_comptime_name.insert(n);

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
        for (const auto &n : u.comptime_fns)
            unit_names.insert(n);
        for (const auto &n : u.macros)
            unit_names.insert(n);
        for (const auto &n : u.comptime_consts)
            unit_names.insert(n);
        for (const auto &n : u.helper_deps)
            unit_names.insert(n);

        struct Span {
            uint32_t off;
            bool in_unit;   ///< pertenece al conjunto comptime (entra al hash).
            bool is_import; ///< es un `import` (viaja, pero NO entra al hash).
        };
        std::vector<Span> spans;
        spans.reserve(mod.decls.size());
        for (const auto &d : mod.decls) {
            if (!d) continue;
            bool in = false;
            if (d->kind == ast::NodeKind::FunctionDecl)
                in =
                    unit_names.count(
                        static_cast<const ast::FunctionDecl *>(d.get())->name) >
                    0;
            else if (d->kind == ast::NodeKind::GlobalVarDecl)
                in = unit_names.count(
                         static_cast<const ast::GlobalVarDecl *>(d.get())
                             ->name) > 0;
            else if (d->kind == ast::NodeKind::ComptimeBlockStmt)
                in = true; // el bloque comptime es parte del conjunto.
            else if (d->kind == ast::NodeKind::ImportDecl)
                /* Los `import` NO son parte del conjunto -- no se ejecutan al
                 * compilar -- pero SI hacen falta para que el texto extraido
                 * compile por si solo: una comptime que usa `string` o llama a
                 * la stdlib necesita las mismas importaciones que su modulo. Se
                 * marcan aparte para no ensuciar el hash: si entrasen en el,
                 * anadir un import que el codigo comptime no usa invalidaria el
                 * cache sin que nada comptime hubiera cambiado. */
                in = false;
            const bool es_import = d->kind == ast::NodeKind::ImportDecl;
            /* El principio REAL, no el que da el AST: si no, el recorte pierde
             * el `comptime`/`@Macro` de esta decl y se lo cuelga a la anterior
             * (ver @ref inicio_real_de_decl). */
            spans.push_back(
                {inicio_real_de_decl(source, d->loc.offset), in, es_import});
        }
        std::sort(spans.begin(), spans.end(),
                  [](const Span &a, const Span &b) { return a.off < b.off; });

        uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis.
        const uint32_t src_len = static_cast<uint32_t>(source.size());
        for (size_t i = 0; i < spans.size(); ++i) {
            if (!spans[i].in_unit && !spans[i].is_import) continue;
            uint32_t start = spans[i].off;
            uint32_t end = (i + 1 < spans.size()) ? spans[i + 1].off : src_len;
            if (start > src_len) start = src_len;
            if (end > src_len) end = src_len;
            /* El TEXTO del conjunto, recogido en el MISMO recorrido que ya
             * calculaba el hash: los spans se computaban, se usaban para
             * hashear y se tiraban, y son justo lo que la fase siguiente
             * necesita para poder compilar el conjunto POR SEPARADO. */
            u.unit_source.append(source, start, end - start);
            if (!spans[i].in_unit) continue; // un import viaja, pero no hashea.
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
    for (const auto &n : u.comptime_fns)
        os << " " << n;
    os << "\n  @Macro         (" << u.macros.size() << "):";
    for (const auto &n : u.macros)
        os << " " << n;
    os << "\n  comptime const (" << u.comptime_consts.size() << "):";
    for (const auto &n : u.comptime_consts)
        os << " " << n;
    os << "\n  helper deps    (" << u.helper_deps.size() << "):";
    for (const auto &n : u.helper_deps)
        os << " " << n;
    os << "\n  content_hash   : 0x" << std::hex << u.content_hash << std::dec
       << "  (clave de cache del artefacto comptime)\n";
}

} // namespace vx
