/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/vx/test_ast_children_scopes.cpp
 * @brief El recorrido de hijos con AMBITO (`vx/ast/children.h`,
 *        `vx/ast/scopes.h`) declara cada nombre donde el lenguaje dice, y el
 *        recorrido por RANURAS sustituye hijos y renombra lo ligado.
 *
 * Cada identificador del caso escrito se resuelve con lo que el recorrido
 * declara y se compara con lo esperado: la sombra de una lambda, de un `match`
 * con guarda, de un `catch`, de un `for-each` (su coleccion NO ve la
 * variable), de un `for` (su `init` no sale) y de un `comptime for`, y el
 * inicializador que lee la variable de fuera.  Que el recorrido no se deja
 * ningun nodo lo comprueba `test_ast_children.cpp` sobre el corpus.
 *
 * Uso:  ./test_vx_test_ast_children_scopes
 */
#include "ast_corpus.h"
#include "vx/ast/children.h"
#include "vx/diagnostic.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace vx::ast;

int g_failures = 0; ///< Cuantas comprobaciones han fallado.

/**
 * @brief Deja constancia si una condicion no se cumple.
 * @param ok   La condicion.
 * @param what Que se estaba comprobando.
 */
void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FALLO: %s\n", what);
        ++g_failures;
    }
}

/**
 * @struct Resolver
 * @brief Resuelve cada identificador con lo que declara el recorrido con
 *        ambito y apunta `nombre@quien:linea` (o `nombre@libre`).
 */
struct Resolver {
    /// @brief Un nombre visible: cual y quien lo liga.
    struct Visible {
        std::string name; ///< el nombre
        std::string tag;  ///< `var:3`, `param:5`...
    };
    std::vector<std::vector<Visible>> scopes; ///< pila de ambitos
    std::vector<std::string> trace;           ///< lo resuelto, en orden
    bool orphan = false; ///< un nombre declarado sin ambito abierto

    /// @brief Se abre un ambito.
    void enter_scope() { scopes.emplace_back(); }

    /// @brief Se cierra el ambito abierto.
    void exit_scope() { scopes.pop_back(); }

    /**
     * @brief Un nombre ligado en el ambito abierto.
     * @param b El nombre y quien lo liga.
     */
    void declare(const Binding &b) {
        if (scopes.empty()) {
            orphan = true;
            return;
        }
        const char *who = "?";
        switch (b.binder ? b.binder->kind : NodeKind::COUNT) {
        case NodeKind::VarDeclStmt: who = "var"; break;
        case NodeKind::ParamDecl: who = "param"; break;
        case NodeKind::ForEachStmt: who = "foreach"; break;
        case NodeKind::ComptimeForStmt: who = "ctfor"; break;
        case NodeKind::TryStmt: who = "catch"; break;
        case NodeKind::MatchExpr: who = "arm"; break;
        default: break;
        }
        scopes.back().push_back(
            {b.name, std::string(who) + ":" + std::to_string(b.loc.line)});
    }

    /**
     * @brief Una expresion: si es un nombre, se resuelve; se baja.
     * @param e La expresion.
     */
    void operator()(const Expr &e) {
        if (e.kind == NodeKind::IdentExpr)
            trace.push_back(resolve(static_cast<const IdentExpr &>(e).name));
        for_each_child_scoped(e, *this);
    }

    /**
     * @brief Cualquier otro hijo: se baja.
     * @param n El hijo.
     */
    template <class T> void operator()(const T &n) {
        for_each_child_scoped(n, *this);
    }

    /**
     * @brief El nombre visible mas interior con ese nombre.
     * @param name El nombre.
     * @return `nombre@quien:linea` o `nombre@libre`.
     */
    std::string resolve(const std::string &name) const {
        for (size_t i = scopes.size(); i-- > 0;)
            for (size_t j = scopes[i].size(); j-- > 0;)
                if (scopes[i][j].name == name)
                    return name + "@" + scopes[i][j].tag;
        return name + "@libre";
    }
};

/**
 * @brief La primera funcion de un modulo con ese nombre.
 * @param mod  Modulo.
 * @param name Nombre.
 * @return La funcion, o nula.
 */
FunctionDecl *find_fn(ModuleNode &mod, const char *name) {
    for (auto &d : mod.decls)
        if (d && d->kind == NodeKind::FunctionDecl &&
            static_cast<FunctionDecl &>(*d).name == name)
            return static_cast<FunctionDecl *>(d.get());
    return nullptr;
}

/**
 * @brief Imprime una traza de resoluciones.
 * @param label Que es.
 * @param trace La traza.
 */
void dump(const char *label, const std::vector<std::string> &trace) {
    std::printf("  %s:", label);
    for (const std::string &s : trace)
        std::printf(" %s", s.c_str());
    std::printf("\n");
}

/**
 * @brief Los ambitos: cada nombre se resuelve donde el lenguaje dice.
 */
void check_scopes() {
    const std::string src =
        "i64 f(i64 p) {\n"                                          // 1
        "    i64 a = 1;\n"                                          // 2
        "    fn(i64) -> i64 g = (i64 a) => { return a + p; };\n"   // 3
        "    i64 b = a;\n"                                          // 4
        "    { i64 b = b + 1; }\n"                                  // 5
        "    i64[3] xs = {1, 2, 3};\n"                              // 6
        "    for (i64 xs : xs) { a = xs; }\n"                       // 7
        "    match (h(a)) { case Uno(a) if a > 0 => { b = a; } "
        "case Otro => { b = a; } }\n"                               // 8
        "    try { b = 1; } catch (MiExc b) { a = b; }\n"           // 9
        "    for (i64 i = 0; i < 3; i = i + 1) { a = i; }\n"        // 10
        "    comptime for (k in 1..3) { a = k; }\n"                 // 11
        "    return i;\n"                                           // 12
        "}\n";
    vx::Diagnostics diags;
    auto mod = vx_tests::parse(src, "<ambitos>", diags);
    check(mod && !diags.has_errors(), "el caso de ambitos parsea");
    if (!mod || diags.has_errors()) return;
    FunctionDecl *f = find_fn(*mod, "f");
    check(f && f->body, "f tiene cuerpo");
    if (!f || !f->body) return;
    Resolver r;
    r(static_cast<const Stmt &>(*f->body));
    const std::vector<std::string> expected = {
        "a@param:3", "p@libre",                  // la lambda tapa `a`
        "a@var:2",                               // tras la lambda, la de fuera
        "b@var:4",                               // su init lee la de fuera
        "xs@var:6", "a@var:2", "xs@foreach:7",   // la coleccion no ve la suya
        "h@libre", "a@var:2",                    // el escrutinio, fuera
        "a@arm:8", "b@var:4", "a@arm:8",         // guarda y cuerpo ven el enlace
        "b@var:4", "a@var:2",                    // otra rama, sin enlace
        "b@var:4", "a@var:2", "b@catch:9",       // el catch tapa `b`
        "i@var:10", "i@var:10", "i@var:10", "a@var:2", "i@var:10",
        "a@var:2", "k@ctfor:11",                 // comptime for
        "i@libre",                               // la `i` del for no sale
    };
    check(!r.orphan, "ningun nombre sin ambito abierto");
    check(r.scopes.empty(), "todos los ambitos se cierran");
    check(r.trace == expected, "cada nombre se resuelve en su ambito");
    if (r.trace != expected) {
        dump("obtenido", r.trace);
        dump("esperado", expected);
    }
}

/**
 * @struct ThisRewriter
 * @brief Reescritura de prueba por ranuras: `x` -> `this.x`, y renombra lo
 *        que se declara `a` a `a2` (el nombre ligado es mutable).
 */
struct ThisRewriter {
    int renamed = 0; ///< nombres ligados renombrados

    /// @brief Se abre un ambito.
    void enter_scope() {}
    /// @brief Se cierra un ambito.
    void exit_scope() {}

    /**
     * @brief Un nombre ligado: `a` pasa a llamarse `a2`.
     * @param b El nombre, mutable.
     */
    void declare(const BindingSlot &b) {
        if (b.name == "a") {
            b.name = "a2";
            ++renamed;
        }
    }

    /**
     * @brief Una expresion: `x` se sustituye; si no, se baja.
     * @param s Su ranura.
     */
    void operator()(ChildSlot<Expr> s) {
        if (s->kind == NodeKind::IdentExpr &&
            static_cast<IdentExpr &>(*s).name == "x") {
            auto fa = std::make_unique<FieldAccessExpr>();
            fa->loc = s->loc;
            fa->base = std::make_unique<ThisExpr>();
            fa->field_name = "x";
            s.replace(std::move(fa));
            return;
        }
        for_each_child_slot_scoped(*s, *this);
    }

    /**
     * @brief Cualquier otra ranura: se baja.
     * @param s La ranura.
     */
    template <class S> void operator()(S s) {
        for_each_child_slot_scoped(*s, *this);
    }
};

/**
 * @brief Las ranuras sustituyen de verdad y el nombre ligado se renombra.
 */
void check_slots() {
    const std::string src = "i64 g() { i64 a = x + 1; return a + x; }\n";
    vx::Diagnostics diags;
    auto mod = vx_tests::parse(src, "<ranuras>", diags);
    check(mod && !diags.has_errors(), "el caso de ranuras parsea");
    if (!mod || diags.has_errors()) return;
    FunctionDecl *g = find_fn(*mod, "g");
    check(g && g->body && g->body->body.size() == 2, "g tiene dos sentencias");
    if (!g || !g->body || g->body->body.size() != 2) return;

    ThisRewriter rw;
    for_each_child_slot_scoped(*g->body, rw);
    check(rw.renamed == 1, "el nombre ligado se renombra una vez");
    const auto &vd = static_cast<const VarDeclStmt &>(*g->body->body[0]);
    check(vd.name == "a2", "la declaracion lleva el nombre nuevo");
    const auto &init = static_cast<const BinaryExpr &>(*vd.init);
    check(init.lhs->kind == NodeKind::FieldAccessExpr,
          "`x` del inicializador pasa a `this.x`");
    const auto &ret = static_cast<const ReturnStmt &>(*g->body->body[1]);
    const auto &sum = static_cast<const BinaryExpr &>(*ret.value);
    check(sum.lhs->kind == NodeKind::IdentExpr &&
              sum.rhs->kind == NodeKind::FieldAccessExpr,
          "solo cambia lo que se sustituye");
}

} // namespace

int main() {
    std::printf("=== test_ast_children_scopes ===\n");
    check_scopes();
    check_slots();
    if (g_failures) {
        std::printf("%d fallos\n", g_failures);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
