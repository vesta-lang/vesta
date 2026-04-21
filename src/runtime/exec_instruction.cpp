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

    void exec_instr_inmed_mov(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst      = instr.data_instruction.inmmed_data.reg;
        uint64_t  imm       = instr.data_instruction.inmmed_data.inmmed;
        uint8_t   direction = instr.flags_info.direction;
        uint8_t   is_signed = instr.flags_info._signed_instruct;

        // Caso especial: registro extendido (rip, rbp, rsp, cur0...)
        if (direction == 1 && is_signed == 1) {
            // si el registro es rip, la instruccion automaticamente esta haciendo un salto.
            write_special(vm, rdst, imm);
            return;
        }

        // Caso 1: mov reg, imm - asignacion directa, sin ALU ni flags
        if (direction == 0 && is_signed == 0) {
            write_reg_table[instr.flags_info.mode](vm, rdst, imm);
            return;
        }

        // Caso 2: mov [reg], imm - escribir inmediato en memoria
        uint64_t addr = vm->registers.regs[rdst].raw();
        size_t   size = Assembly::Bytecode::mode_to_bytes(instr.flags_info.mode);
        vm->vm_mem.write_bytes(addr, &imm, size);
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

    void exec_instr_xchg(ProcessVM *vm, const DecodedInstr &instr) {
        uint64_t val1 = 0;
        uint64_t val2 = 0;

        bool    reg1_is_general = false;
        uint8_t reg1_mode         = 0;
        uint8_t reg1_general      = 0;

        if (instr.data_instruction.regs_data_extent.reg1_flags == 1) {
            // registro especial
            val1              = read_special(vm, instr.data_instruction.regs_data_extent.reg1);
            reg1_is_general = false;
        } else {
            // registro general

            reg1_mode         = instr.data_instruction.regs_data_extent.reg1 >> 4;     // obtenemos el modo
            reg1_general      = instr.data_instruction.regs_data_extent.reg1 & 0b1111; // obtenemos el registro general
            val1              = read_reg_table[reg1_mode](vm, reg1_general);
            reg1_is_general = true;
        }

        if (instr.data_instruction.regs_data_extent.reg2_flags == 1) {
            // registro especial
            val2 = read_special(vm, instr.data_instruction.regs_data_extent.reg2);

            // escribimos el valor obtenido del registro 1 en el registro 2
            write_special(vm, instr.data_instruction.regs_data_extent.reg2, val1);
        } else {
            // registro general
            uint8_t mode = instr.data_instruction.regs_data_extent.reg2 >> 4; // obtenemos el modo

            // obtenemos el registro general
            uint8_t reg_general = instr.data_instruction.regs_data_extent.reg2 & 0b1111;

            val2 = read_reg_table[mode](vm, reg_general);

            // escribimos el valor del registro 1 en el registro 2
            write_reg_table[mode](vm, reg_general, val1);
        }

        // si el registro 1 era general, escribimos el valor del registro 2 en el registro 1
        if (reg1_is_general == 1) {
            // escribimos en el registro general analizado
            write_reg_table[reg1_mode](vm, reg1_general, val2);
        } else {
            // si se dio esta condicion entonces es un registro especial
            write_special(vm, instr.data_instruction.regs_data_extent.reg1, val2);
        }
    }

    void exec_instr_jmp(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cond = instr.data_instruction.inmmed_data.reg;
        const uint64_t addr = instr.data_instruction.inmmed_data.inmmed;
        auto          &fl   = vm->registers.flags.bits;

        bool taken;
        switch (cond) {
            case 0x00: taken = COND_EQ(fl); break;
            case 0x01: taken = COND_NE(fl); break;
            case 0x02: taken = COND_CS(fl); break;
            case 0x03: taken = COND_CC(fl); break;
            case 0x04: taken = COND_MI(fl); break;
            case 0x05: taken = COND_PL(fl); break;
            case 0x06: taken = COND_VS(fl); break;
            case 0x07: taken = COND_VC(fl); break;
            case 0x08: taken = COND_HI(fl); break;
            case 0x09: taken = COND_LS(fl); break;
            case 0x0A: taken = COND_GE(fl); break;
            case 0x0B: taken = COND_LT(fl); break;
            case 0x0C: taken = (fl.ZF == 0 && fl.SF == fl.OF); break;
            case 0x0D: taken = (fl.ZF == 1 || fl.SF != fl.OF); break;
            default:   taken = true; break; // 0x0F y cualquier otro = incondicional
        }

        if (taken)
            write_rip(vm, addr);
    }

    void exec_instr_callvm(ProcessVM *vm, const DecodedInstr &instr) {
        const uint64_t addr     = instr.data_instruction.inmmed_data.inmmed;
        const uint64_t ret_addr = vm->registers.rip.raw() + instr.flags_info.size_instr;

        // push ret_addr
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - 8);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &ret_addr, 8);
        write_rip(vm, addr);
    }

    void exec_instr_ret(ProcessVM *vm, const DecodedInstr &instr) {
        uint64_t ret_addr = 0;
        vm->vm_mem.read_bytes(vm->registers.stack_pointer.raw(), &ret_addr, 8);
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() + 8);
        write_rip(vm, ret_addr);
    }

    void exec_instr_enter(ProcessVM *vm, const DecodedInstr &instr) {
        const uint64_t frame_size = instr.data_instruction.inmmed_data.inmmed;
        const uint64_t rbp_val    = vm->registers.base_pointer.raw();

        // push rbp
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - 8);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &rbp_val, 8);

        // mov rbp, rsp
        vm->registers.base_pointer.raw(vm->registers.stack_pointer.raw());

        // sub rsp, frame_size
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - frame_size);
    }

    void exec_instr_leave(ProcessVM *vm, const DecodedInstr &instr) {
        // mov rsp, rbp
        vm->registers.stack_pointer.raw(vm->registers.base_pointer.raw());

        // pop rbp
        uint64_t rbp_val = 0;
        vm->vm_mem.read_bytes(vm->registers.stack_pointer.raw(), &rbp_val, 8);
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() + 8);
        vm->registers.base_pointer.raw(rbp_val);
    }

    void exec_instr_jmpr(ProcessVM *vm, const DecodedInstr &instr) {
        const uint64_t addr = instr.flags_info.reg_ext
            ? read_special(vm, instr.data_instruction.reg_data.reg1)
            : read_reg64(vm, instr.data_instruction.reg_data.reg1);
        write_rip(vm, addr);
    }

    void exec_instr_callvmr(ProcessVM *vm, const DecodedInstr &instr) {
        const uint64_t addr     = instr.flags_info.reg_ext
            ? read_special(vm, instr.data_instruction.reg_data.reg1)
            : read_reg64(vm, instr.data_instruction.reg_data.reg1);
        const uint64_t ret_addr = vm->registers.rip.raw() + instr.flags_info.size_instr;

        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - 8);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &ret_addr, 8);
        write_rip(vm, addr);
    }

    void exec_instr_jrel(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t cond = instr.data_instruction.inmmed_data.reg;
        const int64_t disp = static_cast<int64_t>(instr.data_instruction.inmmed_data.inmmed);
        auto         &fl   = vm->registers.flags.bits;

        bool taken;
        switch (cond) {
            case 0x00: taken = COND_EQ(fl); break;
            case 0x01: taken = COND_NE(fl); break;
            case 0x02: taken = COND_CS(fl); break;
            case 0x03: taken = COND_CC(fl); break;
            case 0x04: taken = COND_MI(fl); break;
            case 0x05: taken = COND_PL(fl); break;
            case 0x06: taken = COND_VS(fl); break;
            case 0x07: taken = COND_VC(fl); break;
            case 0x08: taken = COND_HI(fl); break;
            case 0x09: taken = COND_LS(fl); break;
            case 0x0A: taken = COND_GE(fl); break;
            case 0x0B: taken = COND_LT(fl); break;
            case 0x0C: taken = (fl.ZF == 0 && fl.SF == fl.OF); break;
            case 0x0D: taken = (fl.ZF == 1 || fl.SF != fl.OF); break;
            default:   taken = true; break;
        }

        if (taken)
            write_rip(vm, vm->registers.rip.raw() + instr.flags_info.size_instr + static_cast<uint64_t>(disp));
    }
}
