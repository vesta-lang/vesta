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
#include "runtime/proceso_runtime.h"
#include "gc/gc_heap.h"
#include "gc/raw_allocator.h"

namespace runtime {

    // -------------------------------------------------------------------------
    // Cursor - acceso a memoria real - 0x00 0xC0 .. 0xC2
    // -------------------------------------------------------------------------

    void exec_instr_readcur(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cur_idx = instr.data_instruction.reg_data.reg2;
        const uint8_t  dst     = instr.data_instruction.reg_data.reg1;
        const uint64_t addr    = vm->registers.cur[cur_idx].qword();

        switch (instr.flags_info.mode) {
            case 0b00: vm->registers.regs[dst].qword(*reinterpret_cast<const uint8_t  *>(addr)); break;
            case 0b01: vm->registers.regs[dst].qword(*reinterpret_cast<const uint16_t *>(addr)); break;
            case 0b10: vm->registers.regs[dst].qword(*reinterpret_cast<const uint32_t *>(addr)); break;
            default:   vm->registers.regs[dst].qword(*reinterpret_cast<const uint64_t *>(addr)); break;
        }
    }

    void exec_instr_writecur(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cur_idx = instr.data_instruction.reg_data.reg2;
        const uint8_t  src     = instr.data_instruction.reg_data.reg1;
        const uint64_t addr    = vm->registers.cur[cur_idx].qword();
        const uint64_t val     = vm->registers.regs[src].qword();

        switch (instr.flags_info.mode) {
            case 0b00: *reinterpret_cast<uint8_t  *>(addr) = static_cast<uint8_t >(val); break;
            case 0b01: *reinterpret_cast<uint16_t *>(addr) = static_cast<uint16_t>(val); break;
            case 0b10: *reinterpret_cast<uint32_t *>(addr) = static_cast<uint32_t>(val); break;
            default:   *reinterpret_cast<uint64_t *>(addr) = val;                        break;
        }
    }

    void exec_instr_gcderef(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cur_idx    = instr.data_instruction.reg_data.reg2;
        const uint8_t  handle_reg = instr.data_instruction.reg_data.reg1;
        const uint64_t raw_handle = vm->registers.regs[handle_reg].qword();

        uint8_t *payload = vm->gc_heap.deref(static_cast<gc::GcHandle>(raw_handle));
        vm->registers.cur[cur_idx].qword(reinterpret_cast<uint64_t>(payload));
    }

    // -------------------------------------------------------------------------
    // GC generacional - 0x00 0xA0 .. 0xA3
    // -------------------------------------------------------------------------

    void exec_instr_newobj(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc = instr.data_instruction.reg_data.reg1;
        const uint64_t size = read_reg_table[instr.flags_info.mode](vm, rsrc);

        gc::GcHandle h = vm->gc_heap.alloc(static_cast<size_t>(size));
        vm->registers.regs[R00].qword(static_cast<uint64_t>(h));
    }

    void exec_instr_gcrun(ProcessVM *vm, const DecodedInstr &instr) {
        (void)instr;
        vm->gc_heap.minor_gc();
    }

    void exec_instr_gcconfig(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc      = instr.data_instruction.reg_data.reg1;
        const uint64_t threshold = read_reg_table[instr.flags_info.mode](vm, rsrc);
        vm->gc_heap.set_old_threshold(static_cast<size_t>(threshold));
    }

    void exec_instr_gc_drop(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc   = instr.data_instruction.reg_data.reg1;
        const uint64_t handle = read_reg_table[instr.flags_info.mode](vm, rsrc);
        vm->gc_heap.drop(static_cast<gc::GcHandle>(handle));
    }

    // -------------------------------------------------------------------------
    // Raw allocator - 0x00 0xB0 .. 0xB2
    // -------------------------------------------------------------------------

    void exec_instr_raw_alloc(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc = instr.data_instruction.reg_data.reg1;
        const uint64_t size = read_reg_table[instr.flags_info.mode](vm, rsrc);

        uint64_t ptr = vm->raw_alloc.alloc(static_cast<size_t>(size));
        vm->registers.regs[R00].qword(ptr);
    }

    void exec_instr_raw_free(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc = instr.data_instruction.reg_data.reg1;
        const uint64_t ptr  = vm->registers.regs[rsrc].qword();
        vm->raw_alloc.free(ptr);
    }

    void exec_instr_raw_realloc(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rptr  = instr.data_instruction.reg_data.reg1;
        const uint8_t  rsize = instr.data_instruction.reg_data.reg2;
        const uint64_t ptr   = vm->registers.regs[rptr].qword();
        const uint64_t size  = vm->registers.regs[rsize].qword();

        uint64_t new_ptr = vm->raw_alloc.realloc(ptr, static_cast<size_t>(size));
        vm->registers.regs[R00].qword(new_ptr);
    }

} // namespace runtime
