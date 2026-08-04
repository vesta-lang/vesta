/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/vm_isa_facts.h
 * @brief HECHOS de la ISA de la VM que el asignador necesita y que no se leen
 *        de los operandos: que registros escribe una instruccion IMPLICITAMENTE
 *        al bajar a bytecode.
 *
 * Es el gemelo de @c operand_roles(MOp) del MachineIR: una tabla por opcode,
 * en UN solo sitio, que el modelo consulta sin saber nada de la VM.
 *
 * ------------------------------------------------------------------------
 * POR QUE EXISTE ESTE FICHERO
 *
 * Muchas instrucciones de la VM dejan su resultado o su estado en R0 sin que R0
 * aparezca como operando: @c deffield deja 1/0, @c defmethod el indice de
 * vtable, @c spawn el pid, @c future el handle, @c msgrecv los bytes leidos, y
 * toda llamada su valor de retorno.  Un valor colocado en R0 que siga vivo
 * despues de una de ellas se PIERDE.
 *
 * Ese conocimiento existia, pero no estaba DICHO en ningun sitio: vivia en el
 * ORDEN del pool del asignador del interprete --
 *
 *     // r0 al final del pool (lo asignamos ultimo para reservarlo para retorno)
 *     std::rotate(...);   // free_pool = [1, 2, ..., 12, 0]
 *
 * -- que lo hacia improbable, no imposible.  Una preferencia escondida en como
 * se recorre un vector no se puede describir ni verificar, y un asignador que
 * reparta por otro criterio la incumple sin enterarse: al conectar el modelo,
 * repartia R0 el primero y rompia cinco programas del corpus (corrupcion de
 * heap en 165_unique_dtor, cuelgues en 39_spawn_pingpong y pic_real).
 *
 * Aqui deja de ser una preferencia y pasa a ser un HECHO comprobable, en el
 * mismo sitio donde se describe la ISA.  Lo que el modelo hace con el es su
 * regla de siempre -- un punto que destruye una lane volatil -- exactamente la
 * misma que aplica a un CALL.  Una llamada es UN CASO de clobber, no la
 * definicion.
 *
 * ------------------------------------------------------------------------
 * COMO SE OBTUVO LA TABLA
 *
 * De la unica fuente que no puede mentir: el propio emisor.  Se extrajeron las
 * @c IrOp que emiten mnemonicos que escriben R0 recorriendo @c ir_emitter.cpp
 * y asociando cada mnemonico al @c case IrOp que lo produce.  La documentacion
 * NO sirve como fuente: puede quedarse atras sin que nada falle.
 *
 * Al anyadir una op que baje a una instruccion con resultado implicito en R0,
 * anyadirla AQUI.  Olvidarlo no da un error de compilacion -- da un valor
 * corrupto cuando el asignador use R0 bajo presion.
 */

#ifndef VESTA_CODEGEN_VM_ISA_FACTS_H
#define VESTA_CODEGEN_VM_ISA_FACTS_H

#include "ir/ssa_ir.h"

namespace codegen {

/**
 * @brief ¿La bajada de @p op a bytecode escribe R0 IMPLICITAMENTE?
 *
 * "Implicitamente" = sin que R0 sea un operando de la instruccion IR, asi que
 * ni la vivacidad ni los roles lo ven.  Incluye el valor de retorno de toda
 * llamada, los codigos de estado de las meta-instrucciones y los handles que
 * devuelven las de concurrencia.
 *
 * CONSERVADOR con el ensamblador embebido (@c RAW_ASM / @c INLINE_ASM /
 * @c ASM_MICRO): su cuerpo es texto arbitrario, asi que puede escribir R0 y no
 * hay forma de saberlo sin parsearlo.  Se asume que lo hace: equivocarse por
 * exceso cuesta un registro; por defecto, un valor corrupto.
 */
inline bool vm_op_clobbers_ret(ir::IrOp op) noexcept {
    switch (op) {
    /* Llamadas: el retorno viaja en R0. */
    case ir::IrOp::CALL:
    case ir::IrOp::CALLN:
    case ir::IrOp::CALLVIRT:
    case ir::IrOp::CALLM:
    case ir::IrOp::CALLSUPER:
    case ir::IrOp::CALLIND:
    case ir::IrOp::CALLCLOSURE:
    case ir::IrOp::TAILCALL:
    case ir::IrOp::PROCEED:

    /* Meta-OOP: dejan estado o resultado en R0 (1/0, indice de vtable, ptr). */
    case ir::IrOp::DEFCLASS:
    case ir::IrOp::DEFFIELD:
    case ir::IrOp::DEFMETHOD:
    case ir::IrOp::ADDADVICE:
    case ir::IrOp::FINDCLASS:
    case ir::IrOp::FINDMETHOD:
    case ir::IrOp::FINDFIELD:
    case ir::IrOp::SPECIALIZE:

    /* Alocacion: el handle / puntero sale en R0. */
    case ir::IrOp::NEWOBJ:
    case ir::IrOp::NEWOBJS:
    case ir::IrOp::GC_ALLOC:
    case ir::IrOp::GC_ALLOCP:
    case ir::IrOp::ARRAY_ALLOC:

    /* Concurrencia y distribucion: pid, handle o cuenta de bytes en R0. */
    case ir::IrOp::SPAWN:
    case ir::IrOp::SPAWN_ON:
    case ir::IrOp::SPAWN_ARGS:
    case ir::IrOp::RSPAWN:
    case ir::IrOp::FUTURE:
    case ir::IrOp::AWAIT:
    case ir::IrOp::FULFILL:
    case ir::IrOp::REJECT:
    case ir::IrOp::MSGSEND:
    case ir::IrOp::MSGRECV:

    /* Carga dinamica de modulos: init_pc en R0. */
    case ir::IrOp::MOD_LOAD:

    /* Liberacion de smart pointers: baja a una llamada (callvm/calln). */
    case ir::IrOp::SMARTPTR_FREE:

    /* Ensamblador embebido: cuerpo opaco -> se asume lo peor. */
    case ir::IrOp::RAW_ASM:
    case ir::IrOp::INLINE_ASM:
    case ir::IrOp::ASM_MICRO:
        return true;
    default:
        return false;
    }
}

} // namespace codegen

#endif // VESTA_CODEGEN_VM_ISA_FACTS_H
