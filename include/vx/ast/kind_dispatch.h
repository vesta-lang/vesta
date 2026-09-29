/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file kind_dispatch.h
 * @brief De la ETIQUETA de un nodo (`NodeKind`) a su CLASE, en un sitio.
 *
 * Cada algoritmo sobre el arbol (clonar, comparar, serializar) necesita pasar
 * de un `const Expr &` al `IntLitExpr` o `CallExpr` que es.  Escribir ese
 * `switch` en cada uno es lo que dejo al clonador viejo sin `super(...)` ni
 * `asm`: le faltaba una rama y devolvia nulo sin decir nada.  Aqui esta una
 * vez; quien lo usa recibe el nodo ya con su clase y, si la etiqueta no es de
 * esa familia, lo sabe (`false`) y lo GRITA.
 *
 * Un nodo nuevo de `ast.h` entra aqui y en su lista de `vx/ast/fields.h`.
 */

#ifndef VX_AST_KIND_DISPATCH_H
#define VX_AST_KIND_DISPATCH_H

#include "vx/ast.h"

namespace vx::ast {

/**
 * @brief Para el compilador con VXE943: un recorrido del arbol recibio un
 *        nodo que no es de la familia esperada.
 *
 * Es un fallo del compilador, no del programa; seguir (devolver nulo, saltar
 * el nodo) haria que la instancia o la copia hiciera OTRA cosa sin avisar.
 *
 * @param n      El nodo.
 * @param family Clase esperada (`Expr`, `Stmt`, `TypeNode`).
 * @param walker Quien recorria (`clone_expr`, `same_parsed`...).
 */
[[noreturn]] void abort_unexpected_node(const Node &n, const char *family,
                                        const char *walker);

/**
 * @brief Llama a @p f con la expresion @p e vista como su clase concreta.
 * @param e Expresion.
 * @param f Visitante con `operator()(const T &)` para cada clase.
 * @return Falso si la etiqueta no es de una expresion.
 */
template <class F> bool visit_expr(const Expr &e, F &f) {
    switch (e.kind) {
    case NodeKind::IntLitExpr: f(static_cast<const IntLitExpr &>(e)); return true;
    case NodeKind::FloatLitExpr: f(static_cast<const FloatLitExpr &>(e)); return true;
    case NodeKind::BoolLitExpr: f(static_cast<const BoolLitExpr &>(e)); return true;
    case NodeKind::NullLitExpr: f(static_cast<const NullLitExpr &>(e)); return true;
    case NodeKind::CharLitExpr: f(static_cast<const CharLitExpr &>(e)); return true;
    case NodeKind::StringLitExpr: f(static_cast<const StringLitExpr &>(e)); return true;
    case NodeKind::IdentExpr: f(static_cast<const IdentExpr &>(e)); return true;
    case NodeKind::FieldAccessExpr: f(static_cast<const FieldAccessExpr &>(e)); return true;
    case NodeKind::BinaryExpr: f(static_cast<const BinaryExpr &>(e)); return true;
    case NodeKind::UnaryExpr: f(static_cast<const UnaryExpr &>(e)); return true;
    case NodeKind::AssignExpr: f(static_cast<const AssignExpr &>(e)); return true;
    case NodeKind::TernaryExpr: f(static_cast<const TernaryExpr &>(e)); return true;
    case NodeKind::TryExpr: f(static_cast<const TryExpr &>(e)); return true;
    case NodeKind::CallExpr: f(static_cast<const CallExpr &>(e)); return true;
    case NodeKind::IndexExpr: f(static_cast<const IndexExpr &>(e)); return true;
    case NodeKind::ThisExpr: f(static_cast<const ThisExpr &>(e)); return true;
    case NodeKind::NewExpr: f(static_cast<const NewExpr &>(e)); return true;
    case NodeKind::SpawnExpr: f(static_cast<const SpawnExpr &>(e)); return true;
    case NodeKind::RSpawnExpr: f(static_cast<const RSpawnExpr &>(e)); return true;
    case NodeKind::LambdaExpr: f(static_cast<const LambdaExpr &>(e)); return true;
    case NodeKind::MatchExpr: f(static_cast<const MatchExpr &>(e)); return true;
    case NodeKind::SuperCallExpr: f(static_cast<const SuperCallExpr &>(e)); return true;
    case NodeKind::SuperMethodCallExpr: f(static_cast<const SuperMethodCallExpr &>(e)); return true;
    case NodeKind::InitListExpr: f(static_cast<const InitListExpr &>(e)); return true;
    case NodeKind::CastExpr: f(static_cast<const CastExpr &>(e)); return true;
    default: return false;
    }
}

/**
 * @brief Llama a @p f con la sentencia @p s vista como su clase concreta.
 * @param s Sentencia.
 * @param f Visitante con `operator()(const T &)` para cada clase.
 * @return Falso si la etiqueta no es de una sentencia.
 */
template <class F> bool visit_stmt(const Stmt &s, F &f) {
    switch (s.kind) {
    case NodeKind::BlockStmt: f(static_cast<const BlockStmt &>(s)); return true;
    case NodeKind::VarDeclStmt: f(static_cast<const VarDeclStmt &>(s)); return true;
    case NodeKind::ExprStmt: f(static_cast<const ExprStmt &>(s)); return true;
    case NodeKind::IfStmt: f(static_cast<const IfStmt &>(s)); return true;
    case NodeKind::WhileStmt: f(static_cast<const WhileStmt &>(s)); return true;
    case NodeKind::DoWhileStmt: f(static_cast<const DoWhileStmt &>(s)); return true;
    case NodeKind::ForStmt: f(static_cast<const ForStmt &>(s)); return true;
    case NodeKind::ReturnStmt: f(static_cast<const ReturnStmt &>(s)); return true;
    case NodeKind::BreakStmt: f(static_cast<const BreakStmt &>(s)); return true;
    case NodeKind::ContinueStmt: f(static_cast<const ContinueStmt &>(s)); return true;
    case NodeKind::GotoStmt: f(static_cast<const GotoStmt &>(s)); return true;
    case NodeKind::LabelStmt: f(static_cast<const LabelStmt &>(s)); return true;
    case NodeKind::TryStmt: f(static_cast<const TryStmt &>(s)); return true;
    case NodeKind::ThrowStmt: f(static_cast<const ThrowStmt &>(s)); return true;
    case NodeKind::ForEachStmt: f(static_cast<const ForEachStmt &>(s)); return true;
    case NodeKind::SynchronizedStmt: f(static_cast<const SynchronizedStmt &>(s)); return true;
    case NodeKind::ComptimeBlockStmt: f(static_cast<const ComptimeBlockStmt &>(s)); return true;
    case NodeKind::ComptimeForStmt: f(static_cast<const ComptimeForStmt &>(s)); return true;
    case NodeKind::AsmStmt: f(static_cast<const AsmStmt &>(s)); return true;
    default: return false;
    }
}

/**
 * @brief Llama a @p f con el tipo escrito @p t visto como su clase concreta.
 * @param t Tipo escrito.
 * @param f Visitante con `operator()(const T &)` para cada clase.
 * @return Falso si la etiqueta no es de un tipo.
 */
template <class F> bool visit_type(const TypeNode &t, F &f) {
    switch (t.kind) {
    case NodeKind::PrimitiveTypeNode: f(static_cast<const PrimitiveTypeNode &>(t)); return true;
    case NodeKind::NamedTypeNode: f(static_cast<const NamedTypeNode &>(t)); return true;
    case NodeKind::PointerTypeNode: f(static_cast<const PointerTypeNode &>(t)); return true;
    case NodeKind::ArrayTypeNode: f(static_cast<const ArrayTypeNode &>(t)); return true;
    case NodeKind::FunctionTypeNode: f(static_cast<const FunctionTypeNode &>(t)); return true;
    case NodeKind::ComputedTypeNode: f(static_cast<const ComputedTypeNode &>(t)); return true;
    default: return false;
    }
}

} // namespace vx::ast

#endif // VX_AST_KIND_DISPATCH_H
