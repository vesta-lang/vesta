/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file field_copy.h
 * @brief LA copia de lo que escribio el parser, para cualquier nodo o pieza
 *        con lista de campos (`vx/ast/fields.h`), con sustitucion de
 *        parametros de tipo.
 *
 * Una declaracion se copiaba en muchos sitios -- la instancia de una funcion,
 * de un struct, de una clase o de un enum genericos, el aplanado de la
 * herencia, la inyeccion de un concepto --, cada uno con su propia lista de
 * "que se copia" escrita a mano, y cada lista se quedaba corta en algo
 * distinto: la instancia de una funcion perdia la direccion de sus parametros
 * y sus contratos, la de un struct que era union, la de un enum los valores de
 * sus variantes.  Aqui la lista es la del nodo, UNA, y la copia la recorre.
 *
 * Lo que un llamante NO quiere de la plantilla (una instancia no es generica:
 * no hereda `type_params`) se quita DESPUES de copiar, a la vista y con su
 * porque.  Lo que el llamante copia a su manera (un cuerpo que solo clona
 * quien se queda la instancia) se nombra al copiar para que no se copie dos
 * veces: `copy_parsed(src, dst, g, &ast::FunctionDecl::body)`.
 */

#ifndef VX_GENERICS_FIELD_COPY_H
#define VX_GENERICS_FIELD_COPY_H

#include "vx/ast.h"
#include "vx/ast/fields.h"
#include "vx/generics/generic_clone.h"

#include <memory>
#include <tuple>
#include <type_traits>
#include <vector>

namespace vx {
namespace vxgen {

/// @namespace vx::vxgen::field_copy
/// @brief La maquinaria de @ref copy_parsed; fuera de aqui se usa esa.
namespace field_copy {

using ast::fields::Field;
using ast::fields::Role;

// Todas las formas de copiar un campo, declaradas ANTES de las plantillas que
// las llaman: dentro de una plantilla solo se ve lo declarado antes.
template <class T, class Skips>
void copy_fields(const T &src, T &dst, const GenSubst &g, const Skips &skip);
template <class V> void copy_value(const V &s, V &d, const GenSubst &g);
template <class V>
void copy_value(const std::vector<V> &s, std::vector<V> &d, const GenSubst &g);
template <class V>
void copy_value(const std::unique_ptr<V> &s, std::unique_ptr<V> &d,
                const GenSubst &g);
void copy_value(const std::unique_ptr<ast::Node> &s,
                std::unique_ptr<ast::Node> &d, const GenSubst &g);
void copy_value(const std::unique_ptr<ast::Expr> &s,
                std::unique_ptr<ast::Expr> &d, const GenSubst &g);
void copy_value(const std::unique_ptr<ast::Stmt> &s,
                std::unique_ptr<ast::Stmt> &d, const GenSubst &g);
void copy_value(const std::unique_ptr<ast::TypeNode> &s,
                std::unique_ptr<ast::TypeNode> &d, const GenSubst &g);
void copy_value(const std::shared_ptr<ast::TypeNode> &s,
                std::shared_ptr<ast::TypeNode> &d, const GenSubst &g);

/**
 * @brief Dos punteros a miembro de clases o tipos distintos nunca nombran el
 *        mismo campo.
 * @return Falso.
 */
template <class P, class Q> constexpr bool same_member(P, Q) { return false; }

/**
 * @brief Dos punteros a miembro del mismo tipo: el mismo campo si son iguales.
 * @param a Uno.
 * @param b Otro.
 * @return Cierto si nombran el mismo miembro.
 */
template <class P> constexpr bool same_member(P a, P b) { return a == b; }

/**
 * @brief Si el miembro @p m esta entre los que el llamante copia a su manera.
 * @param m    El miembro.
 * @param skip Tupla de punteros a miembro que no se copian.
 * @return Cierto si esta.
 */
template <class P, class Skips, size_t... I>
bool is_skipped([[maybe_unused]] P m, [[maybe_unused]] const Skips &skip,
                std::index_sequence<I...>) {
    return (false || ... || same_member(m, std::get<I>(skip)));
}

/**
 * @struct FieldCopier
 * @brief Visitante de la lista de campos de @p T: copia los que escribe el
 *        parser de @c src a @c dst, salvo los de @c skip, y deja los anotados
 *        como estan.
 */
template <class T, class Skips> struct FieldCopier {
    const T &src;       ///< el original
    T &dst;             ///< la copia
    const GenSubst &g;  ///< la sustitucion
    const Skips &skip;  ///< los que copia el llamante

    /**
     * @brief Copia un campo del parser.
     * @param f El campo.
     */
    template <class C, class M>
    void operator()(const Field<Role::Parsed, C, M> &f) const {
        if (is_skipped(f.member, skip,
                       std::make_index_sequence<
                           std::tuple_size<Skips>::value>{}))
            return;
        copy_value(src.*f.member, dst.*f.member, g);
    }

    /**
     * @brief Un campo anotado no se copia: se recalcula sobre la copia, que
     *        puede tener otros tipos.
     */
    template <class C, class M>
    void operator()(const Field<Role::Annotated, C, M> &) const {}
};

/**
 * @brief Copia los campos del parser de @p src en @p dst, salvo los de
 *        @p skip.
 * @param src  Original.
 * @param dst  Destino.
 * @param g    Sustitucion.
 * @param skip Tupla de miembros que no se copian.
 */
template <class T, class Skips>
void copy_fields(const T &src, T &dst, const GenSubst &g, const Skips &skip) {
    FieldCopier<T, Skips> copier{src, dst, g, skip};
    ast::fields::for_each_field<T>(copier);
}

/**
 * @brief Un nodo nuevo de la clase de @p src con sus campos del parser.
 * @param src Original.
 * @param g   Sustitucion.
 * @return La copia.
 */
template <class T>
std::unique_ptr<T> clone_node(const T &src, const GenSubst &g) {
    auto x = std::make_unique<T>();
    copy_fields(src, *x, g, std::tuple<>{});
    return x;
}

/**
 * @brief Un valor sin hijos se copia tal cual; una pieza con lista de campos
 *        (`MatchArm`, `StructFieldDecl`...), campo a campo.
 * @param s Original.
 * @param d Destino.
 * @param g Sustitucion.
 */
template <class V> void copy_value(const V &s, V &d, const GenSubst &g) {
    if constexpr (ast::fields::HasFields<V>::value)
        copy_fields(s, d, g, std::tuple<>{});
    else
        d = s;
}

/**
 * @brief Una lista, elemento a elemento.
 * @param s Original.
 * @param d Destino.
 * @param g Sustitucion.
 */
template <class V>
void copy_value(const std::vector<V> &s, std::vector<V> &d,
                const GenSubst &g) {
    d.clear();
    d.reserve(s.size());
    for (const V &e : s) {
        V x{};
        copy_value(e, x, g);
        d.push_back(std::move(x));
    }
}

/**
 * @brief Un hijo de clase CONOCIDA (un bloque, un parametro, un metodo): no
 *        hace falta despachar por su etiqueta.
 * @param s Original.
 * @param d Destino.
 * @param g Sustitucion.
 */
template <class V>
void copy_value(const std::unique_ptr<V> &s, std::unique_ptr<V> &d,
                const GenSubst &g) {
    static_assert(ast::fields::HasFields<V>::value,
                  "un hijo sin lista de campos no se sabe copiar");
    d = s ? clone_node(*s, g) : nullptr;
}

} // namespace field_copy

/**
 * @brief Copia en @p dst lo que el parser escribio en @p src, con los
 *        parametros de tipo sustituidos segun @p g.
 *
 * @p dst no se vacia antes: lo anotado se queda como este (en una copia
 * recien construida, su valor inicial).
 *
 * @param src  Original.
 * @param dst  Destino.
 * @param g    Sustitucion (vacia: copia literal).
 * @param skip Miembros que el llamante copia a su manera
 *             (`&ast::FunctionDecl::body`); un miembro de @p T o de una base
 *             suya.
 */
template <class T, class... Skip>
void copy_parsed(const T &src, T &dst, const GenSubst &g, Skip... skip) {
    static_assert(ast::fields::HasFields<T>::value,
                  "copy_parsed: el tipo no tiene lista de campos");
    const std::tuple<Skip...> skips{skip...};
    field_copy::copy_fields(src, dst, g, skips);
}

/**
 * @brief Una copia de una pieza sin identidad propia (un campo, una variante).
 * @param src Original.
 * @param g   Sustitucion.
 * @return La copia.
 */
template <class T> T parsed_copy(const T &src, const GenSubst &g) {
    T out{};
    copy_parsed(src, out, g);
    return out;
}

/**
 * @brief Una copia de un NODO de clase conocida.
 * @param src Original.
 * @param g   Sustitucion.
 * @return La copia.
 */
template <class T>
std::unique_ptr<T> clone_parsed(const T &src, const GenSubst &g) {
    return field_copy::clone_node(src, g);
}

/**
 * @brief Una copia de lo que cuelga de una lista de declaraciones (la de un
 *        modulo o un namespace): una declaracion, o un `comptime { ... }` de
 *        nivel superior.
 * @param d Original, o @c nullptr.
 * @param g Sustitucion.
 * @return La copia, o @c nullptr.
 */
std::unique_ptr<ast::Node> clone_decl(const ast::Node *d,
                                      const GenSubst &g = {});

} // namespace vxgen
} // namespace vx

#endif // VX_GENERICS_FIELD_COPY_H
