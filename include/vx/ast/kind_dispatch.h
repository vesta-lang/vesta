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
 * El mismo despacho sirve para un nodo de solo lectura y para uno que se va a
 * reescribir: la constancia del nodo de entrada pasa a la clase concreta.  Dos
 * copias del `switch`, una por constancia, acabarian divergiendo.
 *
 * Un nodo nuevo de `ast.h` entra aqui y en su lista de `vx/ast/fields.h`.
 */

#ifndef VX_AST_KIND_DISPATCH_H
#define VX_AST_KIND_DISPATCH_H

#include "vx/ast.h"

#include <type_traits>

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

namespace dispatch_detail {

/**
 * @brief @p T con la misma constancia que @p From.
 */
template <class T, class From>
using like_const_t =
    std::conditional_t<std::is_const<From>::value, const T, T>;

/**
 * @brief Baja @p b a su clase concreta @p T conservando su constancia.
 * @param b Nodo visto por su familia.
 * @return El mismo nodo visto como @p T.
 */
template <class T, class B> like_const_t<T, B> &as(B &b) {
    return static_cast<like_const_t<T, B> &>(b);
}

} // namespace dispatch_detail

/**
 * @brief Llama a @p f con la expresion @p e vista como su clase concreta.
 * @tparam E `Expr` o `const Expr`: la clase concreta hereda su constancia.
 * @param e Expresion.
 * @param f Visitante con `operator()(T &)` (o `const T &`) para cada clase.
 * @return Falso si la etiqueta no es de una expresion.
 */
template <class E, class F> bool visit_expr(E &e, F &f) {
    static_assert(std::is_same<std::remove_const_t<E>, Expr>::value,
                  "visit_expr recibe un Expr visto por su familia");
    using dispatch_detail::as;
    switch (e.kind) {
    case NodeKind::IntLitExpr: f(as<IntLitExpr>(e)); return true;
    case NodeKind::FloatLitExpr: f(as<FloatLitExpr>(e)); return true;
    case NodeKind::BoolLitExpr: f(as<BoolLitExpr>(e)); return true;
    case NodeKind::NullLitExpr: f(as<NullLitExpr>(e)); return true;
    case NodeKind::CharLitExpr: f(as<CharLitExpr>(e)); return true;
    case NodeKind::StringLitExpr: f(as<StringLitExpr>(e)); return true;
    case NodeKind::IdentExpr: f(as<IdentExpr>(e)); return true;
    case NodeKind::FieldAccessExpr: f(as<FieldAccessExpr>(e)); return true;
    case NodeKind::BinaryExpr: f(as<BinaryExpr>(e)); return true;
    case NodeKind::UnaryExpr: f(as<UnaryExpr>(e)); return true;
    case NodeKind::AssignExpr: f(as<AssignExpr>(e)); return true;
    case NodeKind::TernaryExpr: f(as<TernaryExpr>(e)); return true;
    case NodeKind::TryExpr: f(as<TryExpr>(e)); return true;
    case NodeKind::CallExpr: f(as<CallExpr>(e)); return true;
    case NodeKind::IndexExpr: f(as<IndexExpr>(e)); return true;
    case NodeKind::ThisExpr: f(as<ThisExpr>(e)); return true;
    case NodeKind::NewExpr: f(as<NewExpr>(e)); return true;
    case NodeKind::SpawnExpr: f(as<SpawnExpr>(e)); return true;
    case NodeKind::RSpawnExpr: f(as<RSpawnExpr>(e)); return true;
    case NodeKind::LambdaExpr: f(as<LambdaExpr>(e)); return true;
    case NodeKind::MatchExpr: f(as<MatchExpr>(e)); return true;
    case NodeKind::SuperCallExpr: f(as<SuperCallExpr>(e)); return true;
    case NodeKind::SuperMethodCallExpr: f(as<SuperMethodCallExpr>(e)); return true;
    case NodeKind::InitListExpr: f(as<InitListExpr>(e)); return true;
    case NodeKind::CastExpr: f(as<CastExpr>(e)); return true;
    default: return false;
    }
}

/**
 * @brief Llama a @p f con la sentencia @p s vista como su clase concreta.
 * @tparam S `Stmt` o `const Stmt`: la clase concreta hereda su constancia.
 * @param s Sentencia.
 * @param f Visitante con `operator()(T &)` (o `const T &`) para cada clase.
 * @return Falso si la etiqueta no es de una sentencia.
 */
template <class S, class F> bool visit_stmt(S &s, F &f) {
    static_assert(std::is_same<std::remove_const_t<S>, Stmt>::value,
                  "visit_stmt recibe un Stmt visto por su familia");
    using dispatch_detail::as;
    switch (s.kind) {
    case NodeKind::BlockStmt: f(as<BlockStmt>(s)); return true;
    case NodeKind::VarDeclStmt: f(as<VarDeclStmt>(s)); return true;
    case NodeKind::ExprStmt: f(as<ExprStmt>(s)); return true;
    case NodeKind::IfStmt: f(as<IfStmt>(s)); return true;
    case NodeKind::WhileStmt: f(as<WhileStmt>(s)); return true;
    case NodeKind::DoWhileStmt: f(as<DoWhileStmt>(s)); return true;
    case NodeKind::ForStmt: f(as<ForStmt>(s)); return true;
    case NodeKind::ReturnStmt: f(as<ReturnStmt>(s)); return true;
    case NodeKind::BreakStmt: f(as<BreakStmt>(s)); return true;
    case NodeKind::ContinueStmt: f(as<ContinueStmt>(s)); return true;
    case NodeKind::GotoStmt: f(as<GotoStmt>(s)); return true;
    case NodeKind::LabelStmt: f(as<LabelStmt>(s)); return true;
    case NodeKind::TryStmt: f(as<TryStmt>(s)); return true;
    case NodeKind::ThrowStmt: f(as<ThrowStmt>(s)); return true;
    case NodeKind::ForEachStmt: f(as<ForEachStmt>(s)); return true;
    case NodeKind::SynchronizedStmt: f(as<SynchronizedStmt>(s)); return true;
    case NodeKind::ComptimeBlockStmt: f(as<ComptimeBlockStmt>(s)); return true;
    case NodeKind::ComptimeForStmt: f(as<ComptimeForStmt>(s)); return true;
    case NodeKind::AsmStmt: f(as<AsmStmt>(s)); return true;
    default: return false;
    }
}

/**
 * @brief Llama a @p f con el tipo escrito @p t visto como su clase concreta.
 * @tparam T `TypeNode` o `const TypeNode`: la clase concreta hereda su
 *           constancia.
 * @param t Tipo escrito.
 * @param f Visitante con `operator()(T &)` (o `const T &`) para cada clase.
 * @return Falso si la etiqueta no es de un tipo.
 */
template <class T, class F> bool visit_type(T &t, F &f) {
    static_assert(std::is_same<std::remove_const_t<T>, TypeNode>::value,
                  "visit_type recibe un TypeNode visto por su familia");
    using dispatch_detail::as;
    switch (t.kind) {
    case NodeKind::PrimitiveTypeNode: f(as<PrimitiveTypeNode>(t)); return true;
    case NodeKind::NamedTypeNode: f(as<NamedTypeNode>(t)); return true;
    case NodeKind::PointerTypeNode: f(as<PointerTypeNode>(t)); return true;
    case NodeKind::ArrayTypeNode: f(as<ArrayTypeNode>(t)); return true;
    case NodeKind::FunctionTypeNode: f(as<FunctionTypeNode>(t)); return true;
    case NodeKind::ComputedTypeNode: f(as<ComputedTypeNode>(t)); return true;
    default: return false;
    }
}

/**
 * @brief Llama a @p f con la declaracion @p d vista como su clase concreta.
 *
 * Solo para lo que cuelga de una lista de declaraciones (`ModuleNode::decls`,
 * `NamespaceDecl::decls`).  Un @c ClassMethodDecl lleva la etiqueta
 * @c FunctionDecl, pero nunca vive ahi: esta en las listas TIPADAS de su
 * tipo, que no pasan por aqui.
 *
 * @param d Declaracion.
 * @param f Visitante con `operator()(const T &)` para cada clase.
 * @return Falso si la etiqueta no es de una declaracion (p.ej. un
 *         `comptime { ... }` de nivel superior, que es una sentencia).
 */
template <class F> bool visit_decl(const Node &d, F &f) {
    switch (d.kind) {
    case NodeKind::FunctionDecl: f(static_cast<const FunctionDecl &>(d)); return true;
    case NodeKind::GlobalVarDecl: f(static_cast<const GlobalVarDecl &>(d)); return true;
    case NodeKind::TypeAliasDecl: f(static_cast<const TypeAliasDecl &>(d)); return true;
    case NodeKind::StructDecl: f(static_cast<const StructDecl &>(d)); return true;
    case NodeKind::ClassDecl: f(static_cast<const ClassDecl &>(d)); return true;
    case NodeKind::EnumDecl: f(static_cast<const EnumDecl &>(d)); return true;
    case NodeKind::ExternFnDecl: f(static_cast<const ExternFnDecl &>(d)); return true;
    case NodeKind::ImportDecl: f(static_cast<const ImportDecl &>(d)); return true;
    case NodeKind::NamespaceDecl: f(static_cast<const NamespaceDecl &>(d)); return true;
    case NodeKind::BytesDecl: f(static_cast<const BytesDecl &>(d)); return true;
    case NodeKind::ConceptDecl: f(static_cast<const ConceptDecl &>(d)); return true;
    case NodeKind::ExtensionDecl: f(static_cast<const ExtensionDecl &>(d)); return true;
    case NodeKind::ImplDecl: f(static_cast<const ImplDecl &>(d)); return true;
    default: return false;
    }
}

} // namespace vx::ast

#endif // VX_AST_KIND_DISPATCH_H
