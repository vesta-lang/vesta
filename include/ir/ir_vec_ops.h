/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file ir_vec_ops.h
 * @brief Cuales de las operaciones del IR son las VECTORIALES, y cual de ellas
 *        toca memoria.
 *
 * Media docena de sitios necesitan saberlo: el modelo de efectos, el analisis
 * de escape, las metricas de bucle, el hoisting de invariantes, la reserva del
 * banco de registros anchos y la fusion de direccionamiento del selector.
 * Cada uno lo escribia enumerando los casos a mano, y las listas se habian
 * separado: de las once operaciones que existen, una lista tenia diez, otra
 * diez distintas y otra nueve.
 *
 * Ninguna de esas diferencias daba hoy un resultado equivocado -- dos estan
 * tapadas (@c VEC_BCAST no toca memoria, asi que excluirla de las escrituras
 * es correcto; y @c VEC_FMA_S siempre viene acompanada de un @c VEC_BCAST, que
 * si estaba en la lista de la reserva) --, pero se habian separado SOLAS: al
 * anadir las operaciones con escalar difundido, unas listas se actualizaron y
 * otras no, y nada lo dijo.  Escrita una vez, la proxima operacion vectorial
 * llega a los seis sitios a la vez.
 *
 * Quien necesite un SUBCONJUNTO lo resta aqui explicando por que, en lugar de
 * volver a escribir la lista entera de memoria.
 */

#ifndef VESTA_IR_VEC_OPS_H
#define VESTA_IR_VEC_OPS_H

#include "ir/ssa_ir.h"

namespace ir {

/**
 * @brief Indica si @p op es una de las operaciones vectoriales del IR.
 *
 * Son las once que el vectorizador emite: las tres por elemento (@c VEC_UNOP,
 * @c VEC_BINOP, @c VEC_FMA), sus dos variantes con un escalar difundido
 * (@c VEC_BINOP_S, @c VEC_FMA_S), la difusion del escalar (@c VEC_BCAST) y las
 * cinco del acumulador de una reduccion (@c VEC_ACC_*).
 *
 * @param op Operacion a clasificar.
 * @return true si es vectorial.
 */
inline bool is_vec_op(IrOp op) noexcept {
    switch (op) {
    case IrOp::VEC_UNOP:
    case IrOp::VEC_BINOP:
    case IrOp::VEC_FMA:
    case IrOp::VEC_BINOP_S:
    case IrOp::VEC_FMA_S:
    case IrOp::VEC_BCAST:
    case IrOp::VEC_ACC_ZERO:
    case IrOp::VEC_ACC_ADD:
    case IrOp::VEC_ACC_FMA:
    case IrOp::VEC_ACC_STORE:
    case IrOp::VEC_ACC_COMBINE: return true;
    default: return false;
    }
}

/**
 * @brief Indica si @p op es vectorial Y ademas escribe en memoria.
 *
 * Todas menos @c VEC_BCAST: esa se limita a repartir un escalar por los
 * carriles de un registro, no tiene operando-direccion y no toca memoria.  La
 * distincion importa para no dar por sucia una posicion que nadie escribio --
 * en el hoisting de invariantes de bucle un falso positivo aqui impide sacar
 * del bucle una carga que si era invariante.
 *
 * @param op Operacion a clasificar.
 * @return true si es vectorial y escribe memoria.
 */
inline bool vec_op_writes_memory(IrOp op) noexcept {
    return is_vec_op(op) && op != IrOp::VEC_BCAST;
}

} // namespace ir

#endif // VESTA_IR_VEC_OPS_H
