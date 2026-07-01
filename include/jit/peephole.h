/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/peephole.h
 * @brief Optimizador peephole sobre MachineIR FISICO (post register-alloc),
 *        estilo @c PeepholeOptimizer de LLVM.  Fase SEPARADA del coalescer.
 *
 * Corre tras @c rewrite_to_physical (operandos ya son registros maquina) y
 * antes del encoder.  v1: elimina los SELF-MOVES (`mov rX, rX`) que el
 * coalescing deja al asignar el mismo fisico a dos vregs de una copia.  Es
 * target-neutral (opera sobre MOp/MReg genericos); el JIT, el AOT y todo
 * target futuro lo comparten.
 *
 * Solo borra self-moves 64-bit (`mov r,r` es nop puro) y FP (`movsd/movss
 * x,x` es nop): un `mov e,e` de 32-bit zero-extiende los bits altos y NO es
 * siempre redundante, asi que se conserva (inocuo).
 *
 * Desactivable con @c VESTA_NO_PEEPHOLE=1.
 */

#ifndef VESTA_JIT_PEEPHOLE_H
#define VESTA_JIT_PEEPHOLE_H

#include "jit/machine_ir.h"

namespace jit {

/**
 * @brief Aplica el peephole sobre @p pf (MachineIR fisico).  Modifica
 *        @p pf in-place borrando instrucciones redundantes.
 * @return numero de instrucciones eliminadas.
 */
uint32_t peephole_physical(MFunction &pf);

} // namespace jit

#endif // VESTA_JIT_PEEPHOLE_H
