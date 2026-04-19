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
#include "runtime/decode_instruction.h"

#include "ffi/native_ffi.h"

namespace runtime {
    void exec_instr_hlt(ProcessVM *vm, const DecodedInstr &instr) {
        // bloqueamos el proceso antes de matarlo, ya que si no hacemos esto antes
        // y "matamos el proceso" eñ gestor de procesos finalizara de ejecutar esta
        // instruccion, volvera a la fase de execute y seguira ejecutando lo que quedaba
        // de la instruccion, lo cual genera un fallo fatal al estar la memoria ya liberada.
        vm->scheduler.on_event(EVT_HALT);
        vm->decoded_ptr->flags_info.blocking = true;

        // indicamos que queremos matar el proceso al gestor de procesos.
        //vm->scheduler.kill(vm->pid);
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

    void exec_instr_calln(ProcessVM *vm, const DecodedInstr &instr) {
        void *   fn   = reinterpret_cast<void *>(instr.data_instruction.inmmed_data.inmmed);
        uint64_t argc = instr.data_instruction.inmmed_data.reg; // del ICACHE
        uint64_t r    = 0;

        typedef uint64_t u64;

        // Switch compila a jump table (mismo costo de indirección que dispatch[]),
        // pero cada case llama directamente a fn sin frame intermedio de callN,
        // y solo lee los registros que realmente necesita.
#define A(n) vm->registers.regs[R##n].qword()
        switch (argc) {
            case 0: r = reinterpret_cast<u64(*)()>(fn)();
                break;
            case 1: r = reinterpret_cast<u64(*)(u64)>(fn)(
                    A(01));
                break;
            case 2: r = reinterpret_cast<u64(*)(
                    u64, u64)>(fn)(
                    A(01),A(02));
                break;
            case 3: r = reinterpret_cast<u64(*)(
                    u64, u64, u64)>(fn)(
                    A(01),A(02),A(03));
                break;
            case 4: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04));
                break;
            case 5: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05));
                break;
            case 6: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06));
                break;
            case 7: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06),A(07));
                break;
            case 8: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06),A(07),A(08));
                break;
            case 9: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06),A(07),A(08),A(09));
                break;
            case 10: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06),A(07),A(08),A(09),A(10));
                break;
            case 11: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06),A(07),A(08),A(09),A(10),A(11));
                break;
            case 12: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06),A(07),A(08),A(09),A(10),A(11),A(12));
                break;
            default:
                VM_ASSERT(false, "exec_instr_calln: argc=" + std::to_string(argc) + " excede el maximo de 12", {});
                break; // no eliminar el break, en modo release es lo unico que detiene el switch case
        }
#undef A
        vm->registers.regs[R00].qword(r);
    }
}
