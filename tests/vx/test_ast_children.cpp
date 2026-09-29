/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/vx/test_ast_children.cpp
 * @brief El recorrido unico de hijos (`vx/ast/children.h`) alcanza TODOS los
 *        nodos del arbol, cada uno una vez, en sus tres variantes.
 *
 * Que no falta ningun hijo se comprueba contra una via INDEPENDIENTE: la
 * comparacion de arboles (`same_parsed`), que baja por su cuenta y mira la
 * posicion de cada nodo.  Cada fichero del corpus se parsea dos veces con dos
 * nombres de fichero distintos, A y B: los arboles son iguales salvo en el
 * fichero de cada posicion, asi que la comparacion dice "distintos".  Despues
 * el recorrido mutable pasa por cada nodo de A y le pone el fichero de B.  Si
 * se hubiera saltado un solo nodo, ese conserva el fichero de A y la
 * comparacion lo encuentra.  Ademas, el recorrido de lectura, el de lectura
 * con ambito y el mutable cuentan los mismos nodos, sin repetir ninguno.
 *
 * Tambien: los subarboles que cuelga el comprobador solo cuentan si se piden.
 * Donde se ve cada nombre lo comprueba `test_ast_children_scopes.cpp`.
 *
 * Uso:  ./test_vx_test_ast_children [raiz_del_repositorio]
 */
#include "ast_corpus.h"
#include "vx/ast/ast_equal.h"
#include "vx/ast/children.h"
#include "vx/diagnostic.h"

#include <cstdio>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using namespace vx::ast;

int g_failures = 0; ///< Cuantas comprobaciones han fallado.
int g_bodies = 0;   ///< Cuantos cuerpos se han recorrido.
size_t g_nodes = 0; ///< Cuantos nodos se han alcanzado en total.
int g_differed = 0; ///< Cuerpos que la comparacion distinguia ANTES de marcar.

/**
 * @brief Deja constancia si una condicion no se cumple.
 * @param ok    La condicion.
 * @param what  Que se estaba comprobando.
 * @param where Fichero o caso (puede ir vacio).
 */
void check(bool ok, const char *what, const std::string &where = "") {
    if (!ok) {
        std::printf("FALLO: %s %s\n", what, where.c_str());
        ++g_failures;
    }
}

/**
 * @struct Counter
 * @brief Cuenta, bajando, los nodos que alcanza el recorrido de lectura (con
 *        o sin ambito) y cuantos se repiten.
 */
struct Counter {
    bool scoped = false;                   ///< usar el recorrido con ambito
    size_t count = 0;                      ///< nodos alcanzados
    size_t repeated = 0;                   ///< alcanzados mas de una vez
    int depth = 0;                         ///< ambitos abiertos ahora
    bool unbalanced = false;               ///< se cerro uno que no estaba
    std::unordered_set<const Node *> seen; ///< los ya alcanzados

    /**
     * @brief Un nodo: se cuenta y se baja a sus hijos.
     * @param n El nodo.
     */
    template <class T> void operator()(const T &n) {
        ++count;
        if (!seen.insert(&n).second) ++repeated;
        if (scoped)
            for_each_child_scoped(n, *this);
        else
            for_each_child(n, *this);
    }

    /// @brief Se abre un ambito.
    void enter_scope() { ++depth; }

    /// @brief Se cierra un ambito.
    void exit_scope() {
        if (--depth < 0) unbalanced = true;
    }

    /// @brief Un nombre ligado: debe haber un ambito abierto.
    void declare(const Binding &) {
        if (depth <= 0) unbalanced = true;
    }
};

/**
 * @struct Marker
 * @brief Pasa por cada nodo con el recorrido MUTABLE y le cambia el fichero de
 *        sus posiciones de @c from a @c to.
 *
 * Las piezas sin nodo propio (ramas de `match`, `catch`, operandos de `asm`)
 * tienen su propia posicion y se marcan al pasar por su dueno.
 */
struct Marker {
    const std::string *from = nullptr; ///< fichero de A
    const std::string *to = nullptr;   ///< fichero de B
    size_t count = 0;                  ///< nodos marcados

    /**
     * @brief Cambia el fichero de una posicion si era el de A.
     * @param loc La posicion.
     */
    void retarget(vx::SourceLoc &loc) const {
        if (loc.file_name == from) loc.file_name = to;
    }

    /**
     * @brief Marca un nodo y las piezas que cuelgan de el.
     * @param n El nodo.
     */
    void mark(Node &n) {
        ++count;
        retarget(n.loc);
        if (n.kind == NodeKind::MatchExpr) {
            for (MatchArm &a : static_cast<MatchExpr &>(n).arms)
                retarget(a.loc);
        } else if (n.kind == NodeKind::TryStmt) {
            for (CatchClause &c : static_cast<TryStmt &>(n).catches)
                retarget(c.loc);
        } else if (n.kind == NodeKind::AsmStmt) {
            auto &as = static_cast<AsmStmt &>(n);
            retarget(as.body_loc);
            for (AsmOperand &o : as.operands)
                retarget(o.loc);
        }
    }

    /**
     * @brief Una ranura: se marca su hijo y se baja.
     * @param s La ranura.
     */
    template <class T> void operator()(ChildSlot<T> s) {
        mark(*s);
        for_each_child_slot(*s, *this);
    }

    /**
     * @brief Una ranura compartida: igual.
     * @param s La ranura.
     */
    template <class T> void operator()(SharedChildSlot<T> s) {
        mark(*s);
        for_each_child_slot(*s, *this);
    }
};

/**
 * @brief Cuenta los nodos de @p root y sus descendientes con un recorrido de
 *        lectura.
 * @param root   Raiz.
 * @param scoped Con ambito o sin el.
 * @param where  Para los mensajes.
 * @return La cuenta.
 */
template <class N>
size_t count_nodes(const N &root, bool scoped, const std::string &where) {
    Counter c;
    c.scoped = scoped;
    c(root);
    check(c.repeated == 0, "un nodo alcanzado dos veces", where);
    check(!c.unbalanced && c.depth == 0, "ambitos desequilibrados", where);
    return c.count;
}

/**
 * @brief Compara todo lo de una cosa con cuerpo de A con lo de B.
 * @param a Una.
 * @param b Otra.
 * @return Si todo coincide.
 */
bool same_callable(const vx_tests::Callable &a, const vx_tests::Callable &b) {
    if (a.params->size() != b.params->size()) return false;
    for (size_t i = 0; i < a.params->size(); ++i) {
        const auto &pa = (*a.params)[i];
        const auto &pb = (*b.params)[i];
        if (!pa || !pb) {
            if (pa || pb) return false;
            continue;
        }
        if (!same_parsed(*pa, *pb)) return false;
    }
    return same_parsed(a.ret, b.ret) && same_parsed(a.body, b.body);
}

/**
 * @brief Marca con el recorrido mutable un nodo y todo lo que cuelga de el.
 * @param n Nodo de A.
 * @param m Marcador.
 */
template <class N> void mark_tree(N &n, Marker &m) {
    m.mark(n);
    for_each_child_slot(n, m);
}

/**
 * @brief Marca los parametros y el retorno de una cosa con cuerpo.
 * @param c Cosa con cuerpo de A.
 * @param m Marcador.
 */
void mark_signature(const vx_tests::Callable &c, Marker &m) {
    for (auto &p : *c.params)
        if (p) mark_tree(*p, m);
    if (c.ret) mark_tree(*c.ret, m);
}

/**
 * @struct CorpusFileChecker
 * @brief Visitante de @ref vx_tests::for_each_corpus_file: la prueba de que
 *        el recorrido alcanza todos los nodos, fichero a fichero.
 */
struct CorpusFileChecker {
    /**
     * @brief Un fichero del corpus.
     * @param path Ruta.
     * @param src  Fuente.
     */
    void operator()(const std::string &path, const std::string &src) {
        vx::Diagnostics da;
        vx::Diagnostics db;
        auto ma = vx_tests::parse(src, "A/" + path, da);
        auto mb = vx_tests::parse(src, "B/" + path, db);
        if (!ma || !mb || da.has_errors() || db.has_errors()) return;
        std::vector<vx_tests::Callable> ca;
        std::vector<vx_tests::Callable> cb;
        vx_tests::collect_callables(ma->decls, path, ca);
        vx_tests::collect_callables(mb->decls, path, cb);
        check(ca.size() == cb.size(), "dos parseos, mismas funciones", path);
        if (ca.size() != cb.size()) return;
        for (size_t i = 0; i < ca.size(); ++i)
            check_one(ca[i], cb[i], path);
    }

    /**
     * @brief Una cosa con cuerpo: cuentas iguales en los tres recorridos y,
     *        marcada A, igual a B.
     * @param a    De A.
     * @param b    De B.
     * @param path Fichero (sin el prefijo A/ o B/).
     */
    void check_one(const vx_tests::Callable &a, const vx_tests::Callable &b,
                   const std::string &path) {
        if (!a.body) return;
        ++g_bodies;
        if (!same_callable(a, b)) ++g_differed;
        const size_t plain = count_nodes(*a.body, false, a.where);
        const size_t scoped = count_nodes(*a.body, true, a.where);
        check(plain == scoped, "con ambito se alcanzan los mismos nodos",
              a.where);
        Marker m;
        m.from = util::intern_name("A/" + path);
        m.to = util::intern_name("B/" + path);
        mark_tree(*a.body, m);
        check(m.count == plain, "el recorrido mutable alcanza los mismos nodos",
              a.where);
        mark_signature(a, m);
        g_nodes += plain;
        check(same_callable(a, b),
              "un nodo que el recorrido no alcanzo conserva su fichero",
              a.where);
    }
};

/**
 * @brief Un subarbol que cuelga el comprobador (la expansion de una macro) es
 *        invisible por defecto y visible si se pide.
 */
void check_annotated() {
    CallExpr call;
    call.callee = std::make_unique<IdentExpr>();
    call.macro_expanded = std::make_unique<IntLitExpr>();
    Counter only_parsed;
    for_each_child(call, only_parsed);
    Counter with_annotated;
    for_each_child(call, with_annotated, ChildSet::ParsedAndAnnotated);
    check(only_parsed.count == 1, "por defecto, solo lo escrito");
    check(with_annotated.count == 2, "con anotados, tambien la expansion");
}

} // namespace

int main(int argc, char **argv) {
    std::printf("=== test_ast_children ===\n");
    const std::string root = (argc > 1) ? argv[1] : ".";
    check_annotated();
    CorpusFileChecker checker;
    if (!vx_tests::for_each_corpus_file(root, checker)) {
        std::printf("  no se encontro tests/vx/fmt_corpus.txt\n");
        return 2;
    }
    check(g_bodies > 1000, "el corpus aporta cuerpos de sobra");
    check(g_differed == g_bodies,
          "antes de marcar, la comparacion distingue A de B (no es trivial)");
    std::printf("  %d cuerpos, %zu nodos recorridos\n", g_bodies, g_nodes);
    if (g_failures) {
        std::printf("%d fallos\n", g_failures);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
