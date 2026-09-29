/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file generic_clone_tree.cpp
 * @brief El clon de un arbol con sustitucion de parametros de tipo, sobre la
 *        lista de campos de cada nodo (`vx/ast/fields.h`).
 *
 * El clonador viejo enumeraba a mano, nodo por nodo, lo que copiaba, y se
 * quedaba corto sin que nada avisara: una generica perdia los argumentos con
 * nombre de una llamada (y los pasaba por posicion), el `static` de una
 * variable (que volvia a su valor inicial en cada llamada), los patrones de
 * valor de un `match`, los sufijos de los literales; y `super(...)` o un `asm`
 * desaparecian enteros.  Aqui se copia TODO campo que el parser escribe, y
 * solo esos (con @ref copy_parsed): lo que anota el comprobador se recalcula
 * sobre la instancia.
 *
 * Solo dos cosas no son copiar: un nombre de tipo que es un parametro de la
 * plantilla se SUSTITUYE, en un tipo escrito y en el `new T[...]`.
 */

#include "vx/generics/generic_clone.h"

#include "vx/ast/kind_dispatch.h"
#include "vx/generics/field_copy.h"
#include "vx/generics/generic_infer.h" // type_node_text

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace vx {
namespace vxgen {

namespace {

using field_copy::clone_node;

/**
 * @brief Si @p name es un parametro de tipo de la sustitucion, cual.
 * @param g    Sustitucion.
 * @param name Nombre escrito.
 * @param idx  Salida: su posicion.
 * @return Cierto si lo es.
 */
bool find_type_param(const GenSubst &g, const std::string &name,
                     size_t &idx) {
    if (!g.active()) return false;
    for (size_t i = 0; i < g.params->size(); ++i) {
        if ((*g.params)[i] == name) {
            idx = i;
            return true;
        }
    }
    return false;
}

/**
 * @brief Suma a @p to las marcas de nivel que el tipo escrito @p from llevaba
 *        sobre el parametro: `const T` con `T = i64*` conserva el `const` y lo
 *        que el argumento ya traia.
 * @param from El nombre del parametro tal como se escribio.
 * @param to   Lo que lo sustituye.
 */
void merge_level_marks(const ast::TypeNode &from, ast::TypeNode &to) {
    to.is_nonnull = to.is_nonnull || from.is_nonnull;
    to.is_const = to.is_const || from.is_const;
    to.is_volatile = to.is_volatile || from.is_volatile;
}

/**
 * @struct TypeCloner
 * @brief Visitante de @ref ast::visit_type: clona un tipo escrito y sustituye
 *        los nombres que son parametros de la plantilla.
 */
struct TypeCloner {
    const GenSubst &g;                  ///< la sustitucion
    std::unique_ptr<ast::TypeNode> out; ///< el clon

    /**
     * @brief Cualquier tipo que no es un nombre: sus campos.
     * @param src El original.
     */
    template <class T> void operator()(const T &src) {
        out = clone_node(src, g);
    }

    /**
     * @brief Un nombre: si es un parametro de la plantilla, su argumento.
     * @param src El original.
     */
    void operator()(const ast::NamedTypeNode &src) {
        size_t i = 0;
        if (!find_type_param(g, src.name, i)) {
            out = clone_node(src, g);
            return;
        }
        // El argumento tal como se escribio gana; si no, el tipo resuelto
        // reconstruido ENTERO (puntero con su pointee, array con su tamano).
        out = g.arg_nodes != nullptr
                  ? clone_type_with_subst((*g.arg_nodes)[i])
                  : type_node_from_type((*g.args)[i], src.loc);
        if (out) merge_level_marks(src, *out);
    }
};

/**
 * @struct ExprCloner
 * @brief Visitante de @ref ast::visit_expr: clona una expresion.
 */
struct ExprCloner {
    const GenSubst &g;              ///< la sustitucion
    std::unique_ptr<ast::Expr> out; ///< el clon

    /**
     * @brief Cualquier expresion: sus campos.
     * @param src El original.
     */
    template <class T> void operator()(const T &src) {
        out = clone_node(src, g);
    }

    /**
     * @brief `new T[n]`: el nombre del tipo tambien puede ser un parametro.
     *
     * Sin sustituirlo, la instancia decia `new T[cap]` literal y el
     * comprobador daba "tipo desconocido 'T'".
     *
     * @param src El original.
     */
    void operator()(const ast::NewExpr &src) {
        auto x = clone_node(src, g);
        size_t i = 0;
        if (find_type_param(g, src.class_name, i)) {
            if (g.arg_nodes != nullptr) {
                x->class_name = generics::type_node_text((*g.arg_nodes)[i]);
            } else {
                const Type &a = (*g.args)[i];
                if (a.kind == PrimitiveKind::CLASS ||
                    a.kind == PrimitiveKind::STRUCT)
                    x->class_name = a.struct_name;
                else
                    x->class_name = type_to_string(a);
            }
        }
        out = std::move(x);
    }
};

/**
 * @struct StmtCloner
 * @brief Visitante de @ref ast::visit_stmt: clona una sentencia.
 */
struct StmtCloner {
    const GenSubst &g;              ///< la sustitucion
    std::unique_ptr<ast::Stmt> out; ///< el clon

    /**
     * @brief Cualquier sentencia: sus campos.
     * @param src El original.
     */
    template <class T> void operator()(const T &src) {
        out = clone_node(src, g);
    }
};

} // namespace

std::unique_ptr<ast::TypeNode> clone_type_with_subst(const ast::TypeNode *t,
                                                     const GenSubst &g) {
    if (!t) return nullptr;
    TypeCloner cloner{g, nullptr};
    if (!ast::visit_type(*t, cloner))
        ast::abort_unexpected_node(*t, "TypeNode", "clone_type_with_subst");
    return std::move(cloner.out);
}

std::unique_ptr<ast::ParamDecl> clone_param_with_subst(const ast::ParamDecl &p,
                                                       const GenSubst &g) {
    return clone_node(p, g);
}

std::unique_ptr<ast::Expr> clone_expr(const ast::Expr *e, const GenSubst &g) {
    if (!e) return nullptr;
    ExprCloner cloner{g, nullptr};
    if (!ast::visit_expr(*e, cloner))
        ast::abort_unexpected_node(*e, "Expr", "clone_expr");
    return std::move(cloner.out);
}

std::unique_ptr<ast::Stmt> clone_stmt(const ast::Stmt *s, const GenSubst &g) {
    if (!s) return nullptr;
    StmtCloner cloner{g, nullptr};
    if (!ast::visit_stmt(*s, cloner))
        ast::abort_unexpected_node(*s, "Stmt", "clone_stmt");
    return std::move(cloner.out);
}

} // namespace vxgen
} // namespace vx
