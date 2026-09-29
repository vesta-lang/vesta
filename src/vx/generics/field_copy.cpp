/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file field_copy.cpp
 * @brief Los hijos que hay que DESPACHAR por su etiqueta antes de copiarlos
 *        (una expresion, una sentencia, un tipo, una declaracion), para
 *        @ref vx::vxgen::copy_parsed.
 *
 * El despacho de expresiones, sentencias y tipos -- con la sustitucion de los
 * nombres que son parametros de la plantilla -- vive en
 * `generic_clone_tree.cpp`; aqui solo se enlaza, y se anade el de las
 * declaraciones.
 */

#include "vx/generics/field_copy.h"

#include "vx/ast/kind_dispatch.h"

namespace vx {
namespace vxgen {

namespace {

/**
 * @struct DeclCloner
 * @brief Visitante de @ref ast::visit_decl: copia una declaracion.
 */
struct DeclCloner {
    const GenSubst &g;              ///< la sustitucion
    std::unique_ptr<ast::Node> out; ///< la copia

    /**
     * @brief Cualquier declaracion: sus campos.
     * @param src El original.
     */
    template <class T> void operator()(const T &src) {
        out = field_copy::clone_node(src, g);
    }
};

} // namespace

namespace field_copy {

/// @brief Una declaracion de una lista de declaraciones.
void copy_value(const std::unique_ptr<ast::Node> &s,
                std::unique_ptr<ast::Node> &d, const GenSubst &g) {
    d = clone_decl(s.get(), g);
}

/// @brief Una expresion hija.
void copy_value(const std::unique_ptr<ast::Expr> &s,
                std::unique_ptr<ast::Expr> &d, const GenSubst &g) {
    d = clone_expr(s.get(), g);
}

/// @brief Una sentencia hija.
void copy_value(const std::unique_ptr<ast::Stmt> &s,
                std::unique_ptr<ast::Stmt> &d, const GenSubst &g) {
    d = clone_stmt(s.get(), g);
}

/// @brief Un tipo escrito hijo.
void copy_value(const std::unique_ptr<ast::TypeNode> &s,
                std::unique_ptr<ast::TypeNode> &d, const GenSubst &g) {
    d = clone_type_with_subst(s.get(), g);
}

/// @brief Un tipo escrito COMPARTIDO: cada copia recibe el suyo.
void copy_value(const std::shared_ptr<ast::TypeNode> &s,
                std::shared_ptr<ast::TypeNode> &d, const GenSubst &g) {
    d = std::shared_ptr<ast::TypeNode>(clone_type_with_subst(s.get(), g));
}

} // namespace field_copy

std::unique_ptr<ast::Node> clone_decl(const ast::Node *d, const GenSubst &g) {
    if (d == nullptr) return nullptr;
    DeclCloner cloner{g, nullptr};
    if (ast::visit_decl(*d, cloner)) return std::move(cloner.out);
    /* Un `comptime { ... }` de nivel superior cuelga de la misma lista que las
     * declaraciones, y es una sentencia. */
    if (d->kind == ast::NodeKind::ComptimeBlockStmt)
        return clone_stmt(static_cast<const ast::Stmt *>(d), g);
    ast::abort_unexpected_node(*d, "Decl", "clone_decl");
}

} // namespace vxgen
} // namespace vx
