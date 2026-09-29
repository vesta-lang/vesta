/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file children.h
 * @brief CUALES son los hijos de un nodo, en UN sitio: el recorrido unico de
 *        hijos del arbol, derivado de las listas de campos.
 *
 * Cada analisis que bajaba por el arbol escribia su propio `switch` sobre la
 * etiqueta y enumeraba a mano los hijos de cada nodo.  Casi todos se dejaban
 * alguno sin que nada avisara: el recolector de llamadas comptime no entraba
 * en una asignacion, una lambda ni un `match`; otros olvidaban el `for-each`,
 * el `do-while` o el limite alto de un rango.  Aqui los hijos salen de la
 * lista de campos de cada nodo (`vx/ast/fields.h`), la misma que usan el clon
 * y la comparacion, y los ambitos de `vx/ast/scopes.h`: nadie vuelve a
 * escribir que hijos tiene un nodo ni que nombres liga.
 *
 * Las cuatro variantes recorren los hijos DIRECTOS de @p n (no bajan: bajar
 * es decision del visitante, que se llama a si mismo desde cada hijo), en el
 * orden de la lista de campos.  @p n puede ser un `Expr`, `Stmt`, `TypeNode`
 * o `ParamDecl` visto por su familia (se despacha por su etiqueta; una
 * etiqueta ajena para con VXE943) o por su clase concreta.  Los hijos de un
 * tipo escrito tambien salen de aqui: el tamano de `T[N]` y la expresion de un
 * tipo calculado son hijos `Expr` de su `TypeNode`.
 *
 * **Que es un hijo.**  Un `Expr`, `Stmt` (bloques incluidos), `TypeNode` o
 * `ParamDecl` que cuelga de un campo, directamente, en una lista, o a traves
 * de una pieza con lista (`MatchArm`, `CatchClause`, `AsmOperand`,
 * `ConceptRef`).  Un hijo nulo no se visita.  Por defecto solo cuentan los
 * campos que escribe el parser (@ref ChildSet::Parsed); con
 * @ref ChildSet::ParsedAndAnnotated cuentan ademas los subarboles que cuelga
 * el comprobador (la expansion de una macro, `macro_expanded`; el metodo
 * ligado desazucarado y el inicializador de su receptor en `UnaryExpr`).  No
 * es el defecto a proposito: esos subarboles son un RESULTADO, se recalculan,
 * y quien los recorre lo hace sabiendo en que etapa esta; un analisis sobre lo
 * escrito que los viera contaria dos veces lo mismo.
 *
 * **Visitantes.**
 *   - Lectura (@ref for_each_child, @ref for_each_child_scoped):
 *     `operator()` para `const Expr &`, `const Stmt &`, `const TypeNode &` y
 *     `const ParamDecl &` (puede ser una plantilla).
 *   - Ranuras (@ref for_each_child_slot, @ref for_each_child_slot_scoped):
 *     `operator()` para `ChildSlot<Expr>`, `ChildSlot<Stmt>`,
 *     `ChildSlot<BlockStmt>`, `ChildSlot<TypeNode>`, `ChildSlot<ParamDecl>` y
 *     `SharedChildSlot<TypeNode>` (ver `vx/ast/child_slot.h`).  Una ranura
 *     permite sustituir el hijo; no anadir ni quitar elementos de una lista.
 *   - Con ambito, ademas: `enter_scope()`, `exit_scope()` y
 *     `declare(const Binding &)` (lectura) o `declare(const BindingSlot &)`
 *     (ranuras).  Se avisa en orden: primero los hijos que no ven los nombres,
 *     despues cada ambito que abre el nodo (`enter_scope`, sus `declare`, sus
 *     hijos, `exit_scope`); y tras cada sentencia hija que liga su nombre en
 *     el ambito que la contiene (`T x = ...;`), su `declare`.  Que liga cada
 *     nodo esta en `vx/ast/scopes.h`.
 *
 * Ejemplo (lectura, recursivo):
 *
 *     struct CountCalls {
 *         size_t n = 0;
 *         void operator()(const Expr &e) {
 *             if (e.kind == NodeKind::CallExpr) ++n;
 *             for_each_child(e, *this);
 *         }
 *         template <class T> void operator()(const T &x) {
 *             for_each_child(x, *this);
 *         }
 *     };
 */

#ifndef VX_AST_CHILDREN_H
#define VX_AST_CHILDREN_H

#include "vx/ast/children_walker.h"

#include <type_traits>

namespace vx::ast {

/**
 * @brief Visita los hijos directos de @p n.
 * @param n   Nodo (`Expr`, `Stmt`, `TypeNode`, `ParamDecl` o clase concreta).
 * @param v   Visitante de lectura.
 * @param set Que hijos cuentan.
 */
template <class N, class V>
void for_each_child(const N &n, V &v, ChildSet set = ChildSet::Parsed) {
    children_detail::Walker<children_detail::Access::Read,
                            children_detail::Scoping::Plain, V>
        w(v, set);
    w.node(n);
}

/**
 * @brief Entrega la ranura de cada hijo directo de @p n, para sustituirlo.
 * @param n   Nodo (`Expr`, `Stmt`, `TypeNode`, `ParamDecl` o clase concreta).
 * @param v   Visitante de ranuras.
 * @param set Que hijos cuentan.
 */
template <class N, class V>
void for_each_child_slot(N &n, V &v, ChildSet set = ChildSet::Parsed) {
    static_assert(!std::is_const<N>::value,
                  "for_each_child_slot reescribe: necesita un nodo mutable");
    children_detail::Walker<children_detail::Access::Write,
                            children_detail::Scoping::Plain, V>
        w(v, set);
    w.node(n);
}

/**
 * @brief Visita los hijos directos de @p n avisando de los ambitos que abre
 *        y de los nombres que liga.
 * @param n   Nodo (`Expr`, `Stmt`, `TypeNode`, `ParamDecl` o clase concreta).
 * @param v   Visitante de lectura con `enter_scope`/`exit_scope`/`declare`.
 * @param set Que hijos cuentan.
 */
template <class N, class V>
void for_each_child_scoped(const N &n, V &v,
                           ChildSet set = ChildSet::Parsed) {
    children_detail::Walker<children_detail::Access::Read,
                            children_detail::Scoping::Scoped, V>
        w(v, set);
    w.node(n);
}

/**
 * @brief Entrega la ranura de cada hijo directo de @p n avisando de los
 *        ambitos que abre y de los nombres que liga (con el nombre mutable,
 *        para un renombrador).
 * @param n   Nodo (`Expr`, `Stmt`, `TypeNode`, `ParamDecl` o clase concreta).
 * @param v   Visitante de ranuras con `enter_scope`/`exit_scope`/`declare`.
 * @param set Que hijos cuentan.
 */
template <class N, class V>
void for_each_child_slot_scoped(N &n, V &v, ChildSet set = ChildSet::Parsed) {
    static_assert(!std::is_const<N>::value,
                  "for_each_child_slot_scoped reescribe: necesita un nodo "
                  "mutable");
    children_detail::Walker<children_detail::Access::Write,
                            children_detail::Scoping::Scoped, V>
        w(v, set);
    w.node(n);
}

} // namespace vx::ast

#endif // VX_AST_CHILDREN_H
