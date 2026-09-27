/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file guarded_call.h
 * @brief Una llamada que se hace directa BAJO CONDICION, con la original de
 *        respaldo.
 *
 * Es la forma de toda especulacion y de toda llamada directa que tiene que
 * conservar lo que la indirecta hacia de paso:
 *
 *     B:        ...; br_cond c0, fast0, guard1
 *     guard_n:  br_cond c_n, fast_n, guard_{n+1} | fallback
 *     fast_n:   r_n = call callee_n(fast_operands); br merge
 *     fallback: r_s = <la llamada ORIGINAL>;         br merge
 *     merge:    dst = phi(r_0.., r_s); <el resto de B>
 *
 * El respaldo es la llamada tal cual, asi que da el MISMO resultado -- y lanza
 * lo mismo en el mismo punto -- que sin la transformacion.  Se escribia tres
 * veces: la especulacion del JIT, la estatica y la guarda de nulo del despacho
 * demostrado.
 */

#ifndef IR_PASSES_GUARDED_CALL_H
#define IR_PASSES_GUARDED_CALL_H

#include "ir/ssa_ir.h"

#include <cstddef>

namespace ir {

/**
 * @brief Un camino rapido: si @c cond es cierta, se llama directo a @c callee.
 */
struct GuardedArm {
    IrValueId cond = IR_NO_VALUE; ///< BOOL ya calculado antes de la llamada.
    util::InternedName callee;    ///< a quien se llama directo.
};

/**
 * @brief Parte la llamada @p fn.blocks[@p block].instrs[@p pos] en guardas,
 *        caminos directos y la original de respaldo.
 *
 * Las condiciones tienen que estar YA calculadas en el bloque, antes de
 * @p pos: dominan asi todas las guardas.  No reordena los bloques -- quien
 * parte varios sitios llama a @ref reorder_blocks_rpo al acabar --, y los
 * bloques nuevos van al final, asi que las posiciones ANTERIORES a @p pos en
 * el mismo bloque siguen valiendo.
 *
 * @param fn            Funcion.
 * @param block         Bloque de la llamada.
 * @param pos           Posicion de la llamada en el bloque.
 * @param fast_operands Operandos de los CALL directos.
 * @param arms          Caminos rapidos, en el orden en que se prueban.
 * @param narms         Cuantos hay (al menos uno).
 */
void split_guarded_call(IrFunction &fn, IrBlockId block, size_t pos,
                        const IrOperands &fast_operands, const GuardedArm *arms,
                        size_t narms);

} // namespace ir

#endif // IR_PASSES_GUARDED_CALL_H
