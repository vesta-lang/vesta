/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file inline_bind.h
 * @brief Como se ligan las ENTRADAS de un cuerpo que se copia a la llamada
 *        que lo sustituye.
 *
 * Todo inliner hace lo mismo al empezar: cada parametro del llamado pasa a ser
 * el argumento que le da la llamada, y en una @c CALLCLOSURE la lectura del
 * entorno pasa a ser su operando de entorno.  Y al hacerlo, lo que el llamado
 * SABE de la memoria de esas entradas -- un `out` apunta al anfitrion, el
 * entorno se construyo alli -- pasa al valor del llamante, que puede no
 * saberlo.  Perderlo no da un error: la escritura copiada sale con la
 * instruccion de otra memoria.  Por eso vive aqui una sola vez.
 */

#ifndef IR_PASSES_INLINE_BIND_H
#define IR_PASSES_INLINE_BIND_H

#include "ir/ssa_ir.h"

#include <cstddef>
#include <unordered_map>

namespace ir {

/// Valor del llamado -> valor del llamante al copiar un cuerpo.
using InlineValueMap = std::unordered_map<IrValueId, IrValueId>;

/**
 * @brief Donde empiezan los argumentos de @p call.
 *
 * En una @c CALLCLOSURE el primer operando es el entorno.
 *
 * @param call Llamada.
 * @return Indice del primer argumento.
 */
inline size_t inline_first_arg(const IrInstr &call) noexcept {
    return call.op == IrOp::CALLCLOSURE ? 1u : 0u;
}

/**
 * @brief Cuantos argumentos da @p call.
 * @param call Llamada.
 * @return Numero de argumentos, sin el entorno.
 */
inline size_t inline_arg_count(const IrInstr &call) noexcept {
    const size_t a0 = inline_first_arg(call);
    return call.operands.size() > a0 ? call.operands.size() - a0 : 0u;
}

/**
 * @brief Liga las entradas de @p callee a lo que da @p call, en @p vmap, y
 *        pasa a los valores del llamante lo que el llamado sabe de su memoria.
 *
 * Liga los parametros hasta el menor entre los que declara y los argumentos
 * que llegan: un parametro que no llega (un cast a un `cfn` de menor aridad)
 * se queda sin ligar, y quien copia decide que hacer con el.  En una
 * @c CALLCLOSURE liga ademas cada lectura del entorno.
 *
 * @param caller Funcion que recibe la copia.
 * @param callee Funcion que se copia.
 * @param call   La llamada que se sustituye.
 * @param vmap   Correspondencia de valores, a la que se anade.
 */
void bind_call_inputs(IrFunction &caller, const IrFunction &callee,
                      const IrInstr &call, InlineValueMap &vmap);

/**
 * @brief Si @p in, del cuerpo que se copia, es una lectura del entorno que
 *        @ref bind_call_inputs ya ligo: el copiado la salta.
 * @param call Llamada que se sustituye.
 * @param in   Instruccion del llamado.
 * @return true si hay que saltarla.
 */
inline bool is_bound_env_read(const IrInstr &call, const IrInstr &in) noexcept {
    return call.op == IrOp::CALLCLOSURE &&
           vm_reg_reads_of(in) == VmRegReads::ClosureEnv;
}

} // namespace ir

#endif // IR_PASSES_INLINE_BIND_H
