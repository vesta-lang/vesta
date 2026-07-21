/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file select_simplify.h
 * @brief Canonicalizacion algebraica de SELECT (target-independiente).
 *
 * Simplifica los @c IrOp::SELECT que produce la if-conversion (y los anidados
 * que resultan de ternarios encadenados) a formas mas baratas, ANTES de la
 * politica de lowering.  Reglas (todas reescriben el SELECT in situ, sin crear
 * valores nuevos; DCE/copy-prop limpian lo que quede):
 *   - select(c, x, x)        -> x               (ramas iguales)
 *   - select(true, a, b)     -> a               (cond constante)
 *   - select(false, a, b)    -> b
 *   - select(c, 1, 0)        -> c   (mov/zext)  (c ya es 0/1)
 *   - select(!c, a, b)       -> select(c, b, a) (hunde el NOT; !c = xor c,1)
 *   - select(a<b, a, b)      -> imin(a,b)       (y max/minu/maxu segun sentido)
 *   - select(c, select(c,a,b), d) -> select(c, a, d)  (anidado, misma cond)
 *   - select(c, a, select(c,b,d)) -> select(c, a, d)
 */

#ifndef IR_PASSES_SELECT_SIMPLIFY_H
#define IR_PASSES_SELECT_SIMPLIFY_H

namespace ir {

struct IrFunction;

/**
 * @brief Aplica las simplificaciones algebraicas de SELECT a @p fn.
 * @return Numero de SELECT simplificados/reescritos.
 */
int ir_pass_select_simplify(IrFunction &fn);

} // namespace ir

#endif // IR_PASSES_SELECT_SIMPLIFY_H
