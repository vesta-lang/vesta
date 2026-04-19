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
    // GC generacional — 0x00 0xA0 .. 0xA3
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
    // Raw allocator — 0x00 0xB0 .. 0xB2
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
