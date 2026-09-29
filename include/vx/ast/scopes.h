/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file scopes.h
 * @brief QUE NOMBRES liga cada nodo y DONDE se ven, en un sitio.
 *
 * Todo recorrido que razona sobre nombres -- renombrar, decidir si un `x` es
 * un campo o un local, saber que variables asigna un bucle -- necesita saber
 * que `for (T x : xs) C` liga `x` en `C` y no en `xs`, que el nombre de un
 * `catch` solo se ve en su cuerpo o que una variable local existe desde la
 * sentencia siguiente.  Cada uno lo escribia a mano y cada uno olvidaba una
 * forma distinta: los enlaces de un `match` se tomaban por campos, la
 * variable de un `for-each` no cortaba la sombra.  Aqui se declara una vez,
 * junto a las listas de campos (`vx/ast/fields.h`), y el recorrido con ambito
 * de `vx/ast/children.h` lo aplica.
 *
 * Dos formas de ligar:
 *   - **Ambito propio** (@ref ScopesOf): el nodo abre uno o varios ambitos;
 *     cada uno cubre unos campos del nodo y declara, al abrirse, unos
 *     nombres.  Los campos que no cubre ningun ambito se recorren ANTES, fuera
 *     de todos (asi `xs` no ve la `x` de su propio `for`).
 *   - **Ambito del que la contiene** (@ref BindsInEnclosing): la sentencia
 *     declara su nombre en el ambito donde esta escrita, justo DESPUES de
 *     recorrer sus propios hijos (`i64 x = x + 1;` lee la `x` de fuera).
 *
 * El alcance sigue al comprobador (`type_checker.cpp`, sus `push_scope`): un
 * bloque, un `for` entero (su `init` se ve en la condicion, el paso y el
 * cuerpo), un bloque `comptime`, el cuerpo de un `for-each`, de un
 * `comptime for`, de un `catch`, de una rama de `match` (guarda incluida) y de
 * una lambda.  Una rama de `if` o `while` que no es un bloque NO abre ambito,
 * como en el comprobador.
 *
 * Fuera a proposito: las etiquetas de `goto` (otro espacio de nombres), los
 * operandos de un `asm` (ligan nombres DENTRO del texto ensamblador, que no es
 * arbol) y el receptor sintetico de `UnaryExpr::bound_recv_name` (lo pone el
 * comprobador).
 *
 * Un nodo nuevo que liga nombres entra aqui; uno que no, no hace nada.
 */

#ifndef VX_AST_SCOPES_H
#define VX_AST_SCOPES_H

#include "vx/ast.h"

#include <tuple>
#include <type_traits>

namespace vx::ast::scopes {

/**
 * @struct NoNames
 * @brief Un ambito que no declara nada al abrirse (un bloque: sus nombres
 *        llegan de las sentencias de dentro).
 */
struct NoNames {};

/**
 * @struct Scope
 * @brief Un ambito que abre un nodo: los nombres que declara al abrirse y los
 *        campos del nodo que cubre.
 * @tparam N  Puntero al miembro con los nombres, o @ref NoNames.
 * @tparam Ms Punteros a los miembros cubiertos.
 */
template <class N, class... Ms> struct Scope {
    N names;                  ///< quien trae los nombres
    std::tuple<Ms...> fields; ///< los campos que los ven
};

/**
 * @brief Declara un ambito.
 * @param names  Miembro con los nombres (`std::string`, lista de
 *               `std::string` o lista de `ParamDecl`), o `NoNames{}`.
 * @param fields Miembros cubiertos.
 * @return La descripcion.
 */
template <class N, class... Ms>
constexpr Scope<N, Ms...> scope(N names, Ms... fields) {
    return {names, std::make_tuple(fields...)};
}

/**
 * @struct ScopesOf
 * @brief Los ambitos que abre @p T: `static constexpr auto list()` devuelve
 *        una tupla de @ref Scope.  Sin especializar, ninguno.
 */
template <class T> struct ScopesOf {
    /// @brief Ninguno.  @return Tupla vacia.
    static constexpr auto list() { return std::tuple<>(); }
};

/**
 * @struct BindsInEnclosing
 * @brief Si @p T declara un nombre en el ambito que lo contiene, a partir de
 *        la sentencia siguiente.  Sin especializar, no.
 */
template <class T> struct BindsInEnclosing : std::false_type {};

// -- Ambito del que la contiene ----------------------------------------------

/// `T x = init;`: `x` se ve desde la sentencia siguiente, no en su `init`.
template <> struct BindsInEnclosing<VarDeclStmt> : std::true_type {
    /// @brief El nombre.  @return Puntero al miembro.
    static constexpr auto name() { return &VarDeclStmt::name; }
};

// -- Ambito propio ------------------------------------------------------------

/// `{ ... }`: lo que se declara dentro no sale.
template <> struct ScopesOf<BlockStmt> {
    /// @brief Sus ambitos.  @return La tupla.
    static constexpr auto list() {
        return std::make_tuple(scope(NoNames{}, &BlockStmt::body));
    }
};

/// `comptime { ... }`: su propio ambito, como un bloque.
template <> struct ScopesOf<ComptimeBlockStmt> {
    /// @brief Sus ambitos.  @return La tupla.
    static constexpr auto list() {
        return std::make_tuple(scope(NoNames{}, &ComptimeBlockStmt::stmts));
    }
};

/// `for (init; cond; step) body`: lo del `init` se ve en todo el `for`.
template <> struct ScopesOf<ForStmt> {
    /// @brief Sus ambitos.  @return La tupla.
    static constexpr auto list() {
        return std::make_tuple(scope(NoNames{}, &ForStmt::init, &ForStmt::cond,
                                     &ForStmt::step, &ForStmt::body));
    }
};

/// `for (T x : xs) body`: `x` en el cuerpo; `xs` no la ve.
template <> struct ScopesOf<ForEachStmt> {
    /// @brief Sus ambitos.  @return La tupla.
    static constexpr auto list() {
        return std::make_tuple(
            scope(&ForEachStmt::iter_name, &ForEachStmt::body));
    }
};

/// `comptime for (i in lo..hi) body`: `i` en el cuerpo.
template <> struct ScopesOf<ComptimeForStmt> {
    /// @brief Sus ambitos.  @return La tupla.
    static constexpr auto list() {
        return std::make_tuple(
            scope(&ComptimeForStmt::var_name, &ComptimeForStmt::body));
    }
};

/// `catch (E e) body`: `e` en el cuerpo.
template <> struct ScopesOf<CatchClause> {
    /// @brief Sus ambitos.  @return La tupla.
    static constexpr auto list() {
        return std::make_tuple(
            scope(&CatchClause::var_name, &CatchClause::body));
    }
};

/// `case V(a, b) if guard => body`: los enlaces en la guarda y el cuerpo; el
/// patron de valor no los ve.
template <> struct ScopesOf<MatchArm> {
    /// @brief Sus ambitos.  @return La tupla.
    static constexpr auto list() {
        return std::make_tuple(
            scope(&MatchArm::bindings, &MatchArm::guard, &MatchArm::body));
    }
};

/// `(T a, U b) -> R { body }`: los parametros en el cuerpo; sus tipos y el
/// retorno se leen fuera.
template <> struct ScopesOf<LambdaExpr> {
    /// @brief Sus ambitos.  @return La tupla.
    static constexpr auto list() {
        return std::make_tuple(scope(&LambdaExpr::params, &LambdaExpr::body));
    }
};

} // namespace vx::ast::scopes

#endif // VX_AST_SCOPES_H
