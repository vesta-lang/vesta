/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file inline_bind.cpp
 * @brief Implementacion de @ref ir::bind_call_inputs.
 */

#include "ir/passes/inline_bind.h"

namespace ir {

/**
 * @brief Liga @p callee_v a @p caller_v y le pasa lo que el primero sabe de
 *        su memoria.
 * @param caller   Funcion que recibe la copia.
 * @param callee   Funcion que se copia.
 * @param callee_v Valor del llamado.
 * @param caller_v Valor del llamante que lo sustituye.
 * @param vmap     Correspondencia de valores.
 */
static void bind_one(IrFunction &caller, const IrFunction &callee,
                     IrValueId callee_v, IrValueId caller_v,
                     InlineValueMap &vmap) {
    vmap[callee_v] = caller_v;
    if (caller_v < caller.values.size() && callee_v < callee.values.size())
        caller.values[caller_v].inherit_memory_marks(callee.values[callee_v]);
}

void bind_call_inputs(IrFunction &caller, const IrFunction &callee,
                      const IrInstr &call, InlineValueMap &vmap) {
    const size_t a0 = inline_first_arg(call);
    const size_t n = inline_arg_count(call);
    for (size_t p = 0; p < callee.params.size() && p < n; ++p)
        bind_one(caller, callee, callee.params[p], call.operands[a0 + p],
                 vmap);
    if (call.op != IrOp::CALLCLOSURE || call.operands.empty()) return;
    for (const IrBlock &b : callee.blocks)
        for (const IrInstr &in : b.instrs)
            if (is_bound_env_read(call, in) && in.dst != IR_NO_VALUE)
                bind_one(caller, callee, in.dst, call.operands[0], vmap);
}

} // namespace ir
