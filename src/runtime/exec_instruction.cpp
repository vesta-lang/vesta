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
#include "runtime/exec_instruction.h"

namespace runtime {
    void exec_instr_hlt(ProcessVM *vm, const DecodedInstr &instr) {
        // indicamos que queremos matar la VM, esto hara que la fase
        // EXECUTE emita un evento de tipo EVT_ERROR
        //vm->should_kill = true;
    }

    void exec_instr_push(ProcessVM *vm, const DecodedInstr &instr) {
        uint8_t reg_ext  = instr.flags_info.reg_ext;
        uint8_t reg_code = instr.data_instruction.reg_data.reg1;

        uint64_t value;
        size_t   size;

        if (reg_ext) {
            // Registro especial -> siempre 8 bytes
            value = read_special(vm, reg_code);
            size  = 8;
        } else {
            // Registro general
            uint8_t mode = instr.flags_info.mode;
            size         = Assembly::Bytecode::mode_to_bytes(mode);
            value        = read_reg_table[mode](vm, reg_code);
        }

        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - size);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &value, size);
    }


    void exec_instr_pop(ProcessVM *vm, const DecodedInstr &instr) {
        uint8_t reg_ext  = instr.flags_info.reg_ext;
        uint8_t reg_code = instr.data_instruction.reg_data.reg1;

        size_t size = reg_ext ? 8 : Assembly::Bytecode::mode_to_bytes(instr.flags_info.mode);

        uint64_t value = 0;
        vm->vm_mem.read_bytes(vm->registers.stack_pointer.raw(), &value, size);
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() + size);

        if (reg_ext) {
            write_special(vm, reg_code, value);
        } else {
            uint8_t mode = instr.flags_info.mode;
            write_reg_table[mode](vm, reg_code, value);
        }
    }
}
