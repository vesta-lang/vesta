/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file children_traits.h
 * @brief Lo que el motor del recorrido de hijos sabe de los TIPOS de los
 *        campos (que tipo cuelga hijos) y de los ambitos (que campo cubre cada
 *        uno).  Solo lo incluye `vx/ast/children_walker.h`.
 *
 * Nada aqui nombra un nodo concreto: un hijo es cualquier campo cuyo TIPO
 * cuelga un nodo (`unique_ptr`/`shared_ptr` a una clase del arbol, listas de
 * ellos, o una pieza con su propia lista como `MatchArm`).  Un campo que
 * cuelgue nodos de otra forma (un `optional`, un `map`, un puntero pelado) lo
 * detecta @ref vx::ast::children_detail::MentionsNode y el motor NO COMPILA,
 * en vez de saltarselo en silencio.
 */

#ifndef VX_AST_CHILDREN_TRAITS_H
#define VX_AST_CHILDREN_TRAITS_H

#include "vx/ast.h"
#include "vx/ast/fields.h"
#include "vx/ast/scopes.h"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace vx::ast::children_detail {

/**
 * @struct FamilyOf
 * @brief La familia (`Expr`, `Stmt`, `TypeNode`, `ParamDecl`) de la clase
 *        @p P; `void` si no es de ninguna.
 */
template <class P> struct FamilyOf {
    /// La familia.
    using type = std::conditional_t<
        std::is_base_of<Expr, P>::value, Expr,
        std::conditional_t<
            std::is_base_of<Stmt, P>::value, Stmt,
            std::conditional_t<
                std::is_base_of<TypeNode, P>::value, TypeNode,
                std::conditional_t<std::is_base_of<ParamDecl, P>::value,
                                   ParamDecl, void>>>>;
};

/// @brief Si @p M es un puntero DUENO de un nodo del arbol.
template <class M> struct IsOwnedNode : std::false_type {};
/// @copydoc IsOwnedNode
template <class P, class D>
struct IsOwnedNode<std::unique_ptr<P, D>> : std::is_base_of<Node, P> {};
/// @copydoc IsOwnedNode
template <class P>
struct IsOwnedNode<std::shared_ptr<P>> : std::is_base_of<Node, P> {};

/// @brief Si @p M es una lista de punteros duenos de nodos.
template <class M> struct IsOwnedNodeList : std::false_type {};
/// @copydoc IsOwnedNodeList
template <class E, class A>
struct IsOwnedNodeList<std::vector<E, A>> : IsOwnedNode<E> {};

/// @brief Si @p M es un `shared_ptr`.
template <class M> struct IsShared : std::false_type {};
/// @copydoc IsShared
template <class P> struct IsShared<std::shared_ptr<P>> : std::true_type {};

/**
 * @struct MentionsNode
 * @brief Si el tipo @p M guarda, de la forma que sea, algo del arbol.  Sirve
 *        para que un campo del parser con hijos que el motor no visita sea un
 *        error de COMPILACION, no un hueco silencioso.
 */
template <class M> struct MentionsNode : std::false_type {};
/// @copydoc MentionsNode
template <class P, class D>
struct MentionsNode<std::unique_ptr<P, D>> : std::is_base_of<Node, P> {};
/// @copydoc MentionsNode
template <class P>
struct MentionsNode<std::shared_ptr<P>> : std::is_base_of<Node, P> {};
/// @copydoc MentionsNode
template <class P>
struct MentionsNode<P *> : std::is_base_of<Node, std::remove_cv_t<P>> {};
/// @copydoc MentionsNode
template <class E, class A>
struct MentionsNode<std::vector<E, A>> : MentionsNode<E> {};
/// @copydoc MentionsNode
template <class E> struct MentionsNode<std::optional<E>> : MentionsNode<E> {};
/// @copydoc MentionsNode
template <class A, class B>
struct MentionsNode<std::pair<A, B>>
    : std::integral_constant<bool, MentionsNode<A>::value ||
                                       MentionsNode<B>::value> {};
/// @copydoc MentionsNode
template <class K, class T, class C, class A>
struct MentionsNode<std::map<K, T, C, A>>
    : std::integral_constant<bool, MentionsNode<K>::value ||
                                       MentionsNode<T>::value> {};

/**
 * @struct HoldsChildren
 * @brief Si el motor sabe sacar hijos de @p M: puntero dueno, pieza con
 *        lista, o lista de cualquiera de ellos.
 */
template <class M> struct HoldsChildren {
    /// El veredicto.
    static constexpr bool value =
        IsOwnedNode<M>::value || fields::HasFields<M>::value;
};
/// @copydoc HoldsChildren
template <class E, class A> struct HoldsChildren<std::vector<E, A>> {
    /// El veredicto.
    static constexpr bool value = HoldsChildren<E>::value;
};

/**
 * @struct IsChildList
 * @brief Si @p M es una lista cuyos elementos llevan hijos.  Rasgo aparte y
 *        no `IsVector && HoldsChildren<value_type>`: esa condicion forma
 *        `value_type` tambien cuando @p M no es una lista, y no compila.
 */
template <class M> struct IsChildList : std::false_type {};
/// @copydoc IsChildList
template <class E, class A>
struct IsChildList<std::vector<E, A>>
    : std::integral_constant<bool, HoldsChildren<E>::value> {};

/// @brief Falso que depende de @p T (para un `static_assert` en una rama).
template <class T> struct AlwaysFalse : std::false_type {};

/**
 * @brief Si dos punteros a miembro son el mismo miembro.
 * @param a Uno.
 * @param b Otro (de otro tipo, nunca lo es).
 * @return Cierto si coinciden.
 */
template <class A, class B> constexpr bool same_member(A a, B b) {
    if constexpr (std::is_same<A, B>::value)
        return a == b;
    else
        return false;
}

/**
 * @brief Si el ambito @p sc cubre el miembro @p m.
 * @param sc Ambito.
 * @param m  Miembro.
 * @return Cierto si lo cubre.
 */
template <class Sc, class M, size_t... I>
constexpr bool scope_covers(const Sc &sc, M m, std::index_sequence<I...>) {
    return (false || ... || same_member(std::get<I>(sc.fields), m));
}

/**
 * @brief Si algun ambito de @p list cubre el miembro @p m.
 * @param list Ambitos del nodo.
 * @param m    Miembro.
 * @return Cierto si alguno lo cubre.
 */
template <class L, class M, size_t... I>
constexpr bool any_scope_covers([[maybe_unused]] const L &list,
                                [[maybe_unused]] M m,
                                std::index_sequence<I...>) {
    return (false || ... ||
            scope_covers(std::get<I>(list), m,
                         std::make_index_sequence<std::tuple_size<std::decay_t<
                             decltype(std::get<I>(list).fields)>>::value>{}));
}

/**
 * @struct AllFields
 * @brief Filtro de campos: todos.
 */
struct AllFields {
    /// @brief Admite el campo.  @return Siempre cierto.
    template <class M> static constexpr bool admits(M) { return true; }
};

/**
 * @struct OutsideScopes
 * @brief Filtro de campos de @p T: los que no cubre ninguno de sus ambitos.
 */
template <class T> struct OutsideScopes {
    /// @brief Si @p m no esta en ningun ambito.  @return El veredicto.
    template <class M> static constexpr bool admits(M m) {
        constexpr auto list = scopes::ScopesOf<T>::list();
        return !any_scope_covers(
            list, m,
            std::make_index_sequence<
                std::tuple_size<std::decay_t<decltype(list)>>::value>{});
    }
};

/**
 * @struct InScope
 * @brief Filtro de campos de @p T: los que cubre su ambito @p I.
 */
template <class T, size_t I> struct InScope {
    /// @brief Si @p m esta en el ambito.  @return El veredicto.
    template <class M> static constexpr bool admits(M m) {
        constexpr auto sc = std::get<I>(scopes::ScopesOf<T>::list());
        return scope_covers(
            sc, m,
            std::make_index_sequence<std::tuple_size<
                std::decay_t<decltype(sc.fields)>>::value>{});
    }
};

} // namespace vx::ast::children_detail

#endif // VX_AST_CHILDREN_TRAITS_H
