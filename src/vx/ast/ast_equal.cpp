/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ast_equal.cpp
 * @brief La comparacion de arboles por sus campos del parser.
 */

#include "vx/ast/ast_equal.h"

#include "vx/ast/fields.h"
#include "vx/ast/kind_dispatch.h"

#include <cstring>
#include <memory>
#include <vector>

namespace vx::ast {

namespace {

using fields::Field;
using fields::Role;

// Todas las formas de comparar un campo, declaradas ANTES de las plantillas
// que las llaman.
template <class T> bool fields_equal(const T &a, const T &b);
template <class V> bool same_value(const V &a, const V &b);
template <class V>
bool same_value(const std::vector<V> &a, const std::vector<V> &b);
bool same_value(const double &a, const double &b);
bool same_value(const SourceLoc &a, const SourceLoc &b);
bool same_value(const std::unique_ptr<Expr> &a, const std::unique_ptr<Expr> &b);
bool same_value(const std::unique_ptr<Stmt> &a, const std::unique_ptr<Stmt> &b);
bool same_value(const std::unique_ptr<BlockStmt> &a,
                const std::unique_ptr<BlockStmt> &b);
bool same_value(const std::unique_ptr<TypeNode> &a,
                const std::unique_ptr<TypeNode> &b);
bool same_value(const std::shared_ptr<TypeNode> &a,
                const std::shared_ptr<TypeNode> &b);
bool same_value(const std::unique_ptr<ParamDecl> &a,
                const std::unique_ptr<ParamDecl> &b);

/**
 * @struct FieldComparer
 * @brief Visitante de la lista de campos de @p T: compara los del parser.
 */
template <class T> struct FieldComparer {
    const T &a;     ///< uno
    const T &b;     ///< otro
    bool ok = true; ///< si todo coincide hasta ahora

    /**
     * @brief Compara un campo del parser.
     * @param f El campo.
     */
    template <class C, class M>
    void operator()(const Field<Role::Parsed, C, M> &f) {
        ok = ok && same_value(a.*f.member, b.*f.member);
    }

    /// @brief Un campo anotado no cuenta: es un resultado, no lo escrito.
    template <class C, class M>
    void operator()(const Field<Role::Annotated, C, M> &) {}
};

/**
 * @brief Si dos piezas de la misma clase coinciden campo a campo.
 * @param a Una.
 * @param b Otra.
 * @return Cierto si coinciden.
 */
template <class T> bool fields_equal(const T &a, const T &b) {
    FieldComparer<T> cmp{a, b};
    fields::for_each_field<T>(cmp);
    return cmp.ok;
}

/**
 * @brief Un valor sin hijos, por `==`; una pieza con lista, campo a campo.
 * @param a Uno.
 * @param b Otro.
 * @return Cierto si coinciden.
 */
template <class V> bool same_value(const V &a, const V &b) {
    if constexpr (fields::HasFields<V>::value)
        return fields_equal(a, b);
    else
        return a == b;
}

/**
 * @brief Una lista, elemento a elemento.
 * @param a Una.
 * @param b Otra.
 * @return Cierto si tienen el mismo largo y coinciden en orden.
 */
template <class V>
bool same_value(const std::vector<V> &a, const std::vector<V> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!same_value(a[i], b[i])) return false;
    return true;
}

/// @brief Un literal real, por sus BITS: un NaN escrito es igual a si mismo.
bool same_value(const double &a, const double &b) {
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

/// @brief Una posicion, entera (fichero internado: se compara el puntero).
bool same_value(const SourceLoc &a, const SourceLoc &b) {
    return a.file_name == b.file_name && a.line == b.line &&
           a.column == b.column && a.offset == b.offset &&
           a.length == b.length && a.expansion == b.expansion;
}

/// @brief Una expresion hija.
bool same_value(const std::unique_ptr<Expr> &a,
                const std::unique_ptr<Expr> &b) {
    return same_parsed(a.get(), b.get());
}

/// @brief Una sentencia hija.
bool same_value(const std::unique_ptr<Stmt> &a,
                const std::unique_ptr<Stmt> &b) {
    return same_parsed(a.get(), b.get());
}

/// @brief Un bloque hijo.
bool same_value(const std::unique_ptr<BlockStmt> &a,
                const std::unique_ptr<BlockStmt> &b) {
    if (!a || !b) return !a && !b;
    return fields_equal(*a, *b);
}

/// @brief Un tipo escrito hijo.
bool same_value(const std::unique_ptr<TypeNode> &a,
                const std::unique_ptr<TypeNode> &b) {
    return same_parsed(a.get(), b.get());
}

/// @brief Un tipo escrito compartido.
bool same_value(const std::shared_ptr<TypeNode> &a,
                const std::shared_ptr<TypeNode> &b) {
    return same_parsed(a.get(), b.get());
}

/// @brief Un parametro hijo.
bool same_value(const std::unique_ptr<ParamDecl> &a,
                const std::unique_ptr<ParamDecl> &b) {
    if (!a || !b) return !a && !b;
    return fields_equal(*a, *b);
}

/**
 * @struct SameAs
 * @brief Visitante de los `visit_*`: compara el nodo visitado con @c other,
 *        que ya se sabe de la misma etiqueta.
 * @tparam Base `Expr`, `Stmt` o `TypeNode`.
 */
template <class Base> struct SameAs {
    const Base &other; ///< el otro nodo
    bool eq = false;   ///< el veredicto

    /**
     * @brief Compara dos nodos de la clase @p T.
     * @param a El visitado.
     */
    template <class T> void operator()(const T &a) {
        eq = fields_equal(a, static_cast<const T &>(other));
    }
};

} // namespace

bool same_parsed(const Expr *a, const Expr *b) {
    if (!a || !b) return !a && !b;
    if (a->kind != b->kind) return false;
    SameAs<Expr> cmp{*b};
    if (!visit_expr(*a, cmp)) abort_unexpected_node(*a, "Expr", "same_parsed");
    return cmp.eq;
}

bool same_parsed(const Stmt *a, const Stmt *b) {
    if (!a || !b) return !a && !b;
    if (a->kind != b->kind) return false;
    SameAs<Stmt> cmp{*b};
    if (!visit_stmt(*a, cmp)) abort_unexpected_node(*a, "Stmt", "same_parsed");
    return cmp.eq;
}

bool same_parsed(const TypeNode *a, const TypeNode *b) {
    if (!a || !b) return !a && !b;
    if (a->kind != b->kind) return false;
    SameAs<TypeNode> cmp{*b};
    if (!visit_type(*a, cmp))
        abort_unexpected_node(*a, "TypeNode", "same_parsed");
    return cmp.eq;
}

bool same_parsed(const ParamDecl &a, const ParamDecl &b) {
    return fields_equal(a, b);
}

} // namespace vx::ast
