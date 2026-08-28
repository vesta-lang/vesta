/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/ssa_coalesce.h
 * @brief Coalescing de PHI basado en SSA (Sreedhar et al.), sobre el
 *        @c ir::IrFunction ANTES de la eliminacion de PHI.
 *
 * = Por que SSA y no MachineIR =
 *
 * Coalescer los phi-copies (contadores/acumuladores de loop: `i++`, `acc+=`)
 * es el mayor win de codegen -- elimina 2-4 movs POR ITERACION.  Pero hacerlo
 * sobre la MachineIR (tras out-of-SSA) es UNSOUND: alli el phi-dst es
 * MULTI-DEF (reasignado en cada predecesor) y el modelo de intervalos es
 * conservador (rango de bloque completo), lo que oculta interferencias reales
 * -> miscompila.
 *
 * En el IR (SSA) cada valor es SINGLE-DEF, asi que la liveness es PRECISA (con
 * huecos exactos).  Un phi-dst y su phi-arg NO interfieren cuando el dst-viejo
 * muere justo donde el arg se define (el caso del contador de loop): el mismo
 * registro los sostiene sin conflicto.  Coalescer aqui es SOUND.
 *
 * = Que hace =
 *
 * Para cada `dst = phi((arg_i, pred_i), ...)`, intenta unir dst con cada arg_i
 * (union-find) si el CLUSTER resultante no tiene interferencia interna (via
 * liveness SSA precisa, esquema use=2*gi / def=2*gi+1) y no viola la
 * restriccion 2-address (dst<->operands[1] de un ALU se legaliza a
 * `mov dst,op0; OP dst,op1`; unir dst con op1 clobberearia op1).  Devuelve un
 * remap `valor -> representante` que el backend aplica a los vregs: los
 * phi-copies coalescidos se vuelven self-moves (los borra el peephole).
 *
 * Comparte backend con el JIT y el AOT (se aplica sobre la MachineIR de
 * ambos); target-neutral.  Gated por @c VESTA_JIT_COALESCE=1 mientras madura.
 */

#ifndef VESTA_JIT_SSA_COALESCE_H
#define VESTA_JIT_SSA_COALESCE_H

#include "ir/ssa_ir.h"
#include "jit/machine_ir.h"
#include "jit/sched/machine_effects.h" // EffIsa: para quien se genera este codigo

#include <cstdint>
#include <vector>

namespace jit {

/**
 * @brief Calcula el remap de coalescing de PHI para @p fn (SSA).
 * @return remap indexado por IrValueId (0..fn.values.size()-1); remap[v] es
 *         el valor REPRESENTANTE del cluster de v (remap[v]==v si no se
 *         coalesce).  Vacio si no hay nada que coalescer.
 */
/**
 * @brief Que le hace el objetivo al destino de un binop ALU.
 *
 * Es lo unico que este pase necesita saber del objetivo, y por eso se pregunta
 * asi y no por la ISA: el emisor de bytecode tampoco es una ISA de la base de
 * instrucciones, y tenia el mismo derecho a contestar.
 */
enum class DstKind : uint8_t {
    /// Dos direcciones: el destino es tambien operando leido.  Un binop de tres
    /// operandos del IR se legaliza a `mov dst, op0; OP dst, op1`, y ese `mov`
    /// obliga a que dst no comparta registro con op1.  Es el caso de x86.
    Destructive,
    /// Tres direcciones: `add x0, x1, x2` no toca ni x1 ni x2.  No hay `mov`
    /// previo, luego tampoco la restriccion.  arm64, arm32 y RISC-V.
    Preserving,
};

/// @brief Como trata @p isa el destino de un binop ALU.
DstKind dst_kind_of_isa(sched::EffIsa isa) noexcept;

std::vector<uint32_t> ssa_phi_coalesce_remap(const ir::IrFunction &fn,
                                             DstKind dst);

/**
 * @brief Aplica el coalescing SSA a la MachineIR @p mf de la funcion @p fn.
 *
 * Reescribe cada operando vreg de @p mf cuyo id corresponde a un IrValueId
 * (id < fn.values.size()) a su representante segun @c ssa_phi_coalesce_remap.
 * Los vregs temporales (id >= fn.values.size()) no se tocan.  Las copias de
 * PHI coalescidas quedan como `MOV vX, vX` (las elimina el peephole).
 *
 * @p dst es OBLIGATORIO porque una de las restricciones que impone este pase --
 * que el destino de un binop no comparta registro con su segundo operando --
 * solo existe donde la instruccion es de DOS DIRECCIONES.  Donde la ALU es de
 * tres, esa restriccion no protege de nada: solo impide coalescings que si eran
 * validos.  Se pregunta, no se supone.
 *
 * @return true si se reescribio algo (el caller DEBE reconstruir intervalos).
 */
bool apply_ssa_coalesce(MFunction &mf, const ir::IrFunction &fn, DstKind dst);

} // namespace jit

#endif // VESTA_JIT_SSA_COALESCE_H
