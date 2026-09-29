/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file fields.h
 * @brief QUE CAMPOS tiene cada nodo del arbol, en UN sitio.
 *
 * Cada recorrido del arbol -- clonar, comparar, serializar -- enumeraba los
 * campos de cada nodo a su manera, y se quedaban cortos sin que nadie lo
 * notara: el clonador de las genericas olvidaba los argumentos con nombre, el
 * `static` de una variable y los patrones de valor de un `match`, y la
 * instancia hacia OTRA cosa que la plantilla.
 *
 * Aqui cada nodo declara su lista de campos (`Of<T>::list()`), y cada campo
 * dice su PAPEL: lo escribio el PARSER (es parte de lo que el programador
 * escribio: se copia, se compara, viaja) o lo ANOTO despues el comprobador o
 * la bajada (es un resultado: se recalcula).  Los algoritmos recorren la lista;
 * nadie vuelve a escribir que campos tiene un nodo.
 *
 * Un campo nuevo en un nodo de `ast.h` va tambien a su lista, en
 * `fields_types.h`, `fields_exprs.h`, `fields_stmts.h`, `fields_decls.h` o
 * `fields_type_decls.h`.
 */

#ifndef VX_AST_FIELDS_H
#define VX_AST_FIELDS_H

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "vx/ast.h"

namespace vx::ast::fields {

/**
 * @enum Role
 * @brief Quien escribe un campo.
 */
enum class Role : uint8_t {
    Parsed,    ///< lo escribe el parser: es parte del programa escrito
    Annotated, ///< lo anota el comprobador o la bajada: se recalcula
};

/**
 * @struct Field
 * @brief Un campo de un nodo: su papel (en el TIPO, para que un algoritmo que
 *        no toca los anotados ni siquiera los instancie) y su miembro.
 * @tparam R Papel.
 * @tparam C Clase que declara el miembro (puede ser una base del nodo).
 * @tparam M Tipo del miembro.
 */
template <Role R, class C, class M> struct Field {
    M C::*member;     ///< el miembro
    const char *name; ///< su nombre, para diagnosticos y volcados
};

/**
 * @brief Un campo que escribe el parser.
 * @param m Puntero al miembro.
 * @param n Nombre del campo.
 * @return La descripcion.
 */
template <class C, class M>
constexpr Field<Role::Parsed, C, M> parsed(M C::*m, const char *n) {
    return {m, n};
}

/**
 * @brief Un campo que anota el comprobador o la bajada.
 * @param m Puntero al miembro.
 * @param n Nombre del campo.
 * @return La descripcion.
 */
template <class C, class M>
constexpr Field<Role::Annotated, C, M> annotated(M C::*m, const char *n) {
    return {m, n};
}

/**
 * @struct Of
 * @brief La lista de campos de @p T: `static constexpr auto list()` devuelve
 *        una tupla de @ref Field.  Se especializa por nodo; un tipo sin
 *        especializacion no tiene lista.
 */
template <class T> struct Of;

/**
 * @struct HasFields
 * @brief Si @p T tiene lista de campos.
 */
template <class T, class = void> struct HasFields : std::false_type {};

/// @copydoc HasFields
template <class T>
struct HasFields<T, std::void_t<decltype(Of<T>::list())>> : std::true_type {};

/**
 * @brief Llama a @p f con cada campo de la tupla, en orden.
 * @param f Visitante.
 * @param t Tupla de campos.
 */
template <class F, class Tuple, size_t... I>
void apply_each(F &f, const Tuple &t, std::index_sequence<I...>) {
    (f(std::get<I>(t)), ...);
}

/**
 * @brief Recorre los campos de @p T en el orden de su lista.
 *
 * @p f recibe cada @ref Field; que haga con el lo decide el algoritmo (copiar
 * el miembro de un nodo a otro, compararlo, escribirlo).
 *
 * @tparam T Nodo o estructura auxiliar con lista.
 * @param f Visitante.
 */
template <class T, class F> void for_each_field(F &f) {
    constexpr auto list = Of<T>::list();
    apply_each(f, list,
               std::make_index_sequence<
                   std::tuple_size<std::decay_t<decltype(list)>>::value>{});
}

} // namespace vx::ast::fields

#include "vx/ast/fields_types.h"
#include "vx/ast/fields_exprs.h"
#include "vx/ast/fields_stmts.h"
#include "vx/ast/fields_decls.h"
#include "vx/ast/fields_type_decls.h"

#endif // VX_AST_FIELDS_H
