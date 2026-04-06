/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */
#ifndef DISPATCH_TABLE_H
#define DISPATCH_TABLE_H

#define DISPATCH() \
    goto *dispatch_table[vm->decoded_ptr->is_not_extended][vm->decoded_ptr->opcode_index]

#define NEXT() \
    if (!decoded_ptr->did_jump) \
    rip.ptr_vm.raw += decoded_ptr->size; \
    decode_instruction(); \
    DISPATCH()
#include "exec_instruction.h"
#include "runtime.h"

namespace runtime {
    class VM;
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpedantic"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
/*
inline void execute_instr(runtime::VM *vm, const runtime::DecodedInstr &instr) {
    static void *dispatch_table[2][0x100] = {nullptr};
    static bool initialized = false;

    if (!initialized) {
        // primarios
        dispatch_table[0][0x00] = &&OP_NOP;
        // extendidos
        dispatch_table[0][0x05] = &&OP_ADD_reg;

        initialized = true;
    }

    DISPATCH();
OP_NOP:
OP_ADD_reg:
    runtime::exec_instr_add_reg(vm, instr);

OP_INVALID:
    // instrucción inválida
    return;
}*/


#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif //DISPATCH_TABLE_H
