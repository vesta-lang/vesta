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
#ifndef EXEC_INSTRUCTION_H
#define EXEC_INSTRUCTION_H
#include <cstdint>
#include "decode_table.h"
#include "emmit/emmit_decl.h"

namespace runtime {
    static constexpr uint64_t MASKS[4] = {
        0xFFULL,              // 8 bits
        0xFFFFULL,            // 16 bits
        0xFFFFFFFFULL,        // 32 bits
        0xFFFFFFFFFFFFFFFFULL // 64 bits
    };

    static constexpr int SIGNBIT[4] = {7, 15, 31, 63};

    using ReadRegFn = uint64_t(*)(ProcessVM *, uint8_t);

    static uint64_t read_reg8(ProcessVM *vm, uint8_t r) {
        return vm->registers.regs[r].byte_lo();
    }

    static uint64_t read_reg16(ProcessVM *vm, uint8_t r) {
        return vm->registers.regs[r].word_lo();
    }

    static uint64_t read_reg32(ProcessVM *vm, uint8_t r) {
        return vm->registers.regs[r].dword_lo();
    }

    static uint64_t read_reg64(ProcessVM *vm, uint8_t r) {
        return vm->registers.regs[r].qword();
    }

    static constexpr ReadRegFn read_reg_table[] = {
        read_reg8,
        read_reg16,
        read_reg32,
        read_reg64
    };

    using WriteRegFn = void(*)(ProcessVM *, uint8_t, uint64_t);

    static void write_reg8(ProcessVM *vm, uint8_t r, uint64_t v) {
        vm->registers.regs[r].byte_lo(v);
    }

    static void write_reg16(ProcessVM *vm, uint8_t r, uint64_t v) {
        vm->registers.regs[r].word_lo(v);
    }

    static void write_reg32(ProcessVM *vm, uint8_t r, uint64_t v) {
        vm->registers.regs[r].dword_lo(v);
    }

    static void write_reg64(ProcessVM *vm, uint8_t r, uint64_t v) {
        vm->registers.regs[r].qword(v);
    }

    static constexpr WriteRegFn write_reg_table[] = {
        write_reg8,
        write_reg16,
        write_reg32,
        write_reg64
    };

    using ReadSpecialFn = uint64_t(*)(ProcessVM *);

    static uint64_t read_cur0(ProcessVM *vm) {
        return vm->registers.cur[0].qword();
    }

    static uint64_t read_cur1(ProcessVM *vm) {
        return vm->registers.cur[1].qword();
    }

    static uint64_t read_cur2(ProcessVM *vm) {
        return vm->registers.cur[2].qword();
    }

    static uint64_t read_cur3(ProcessVM *vm) {
        return vm->registers.cur[3].qword();
    }

    static uint64_t read_rip(ProcessVM *vm) {
        return vm->registers.rip.raw();
    }

    static uint64_t read_rbp(ProcessVM *vm) {
        return vm->registers.base_pointer.raw();
    }

    static uint64_t read_rsp(ProcessVM *vm) {
        return vm->registers.stack_pointer.raw();
    }

    static uint64_t read_rflags(ProcessVM *vm) {
        return vm->registers.flags.raw;
    }

    static constexpr ReadSpecialFn read_special_table[12] = {
        read_cur0,  // 0
        read_cur1,  // 1
        read_cur2,  // 2
        read_cur3,  // 3
        nullptr,    // 4
        nullptr,    // 5
        nullptr,    // 6
        nullptr,    // 7
        read_rip,   // 8
        read_rbp,   // 9
        read_rsp,   // 10
        read_rflags // 11
    };


    inline uint64_t read_special(ProcessVM *vm, uint8_t code) {
        if (code >= 12 || read_special_table[code] == nullptr) {
            return 0;
        }
        return read_special_table[code](vm);
    }

    using WriteSpecialFn = void(*)(ProcessVM *, uint64_t);

    static void write_cur0(ProcessVM *vm, uint64_t v) {
        vm->registers.cur[0].qword(v);
    }

    static void write_cur1(ProcessVM *vm, uint64_t v) {
        vm->registers.cur[1].qword(v);
    }

    static void write_cur2(ProcessVM *vm, uint64_t v) {
        vm->registers.cur[2].qword(v);
    }

    static void write_cur3(ProcessVM *vm, uint64_t v) {
        vm->registers.cur[3].qword(v);
    }

    static void write_rip(ProcessVM *vm, uint64_t v) {
        vm->registers.rip.raw(v);
        // al modificar rip, se esta haciendo un salto
        vm->decoded_ptr->flags_info.did_jump = true;
    }

    static void write_rbp(ProcessVM *vm, uint64_t v) {
        vm->registers.base_pointer.raw(v);
    }

    static void write_rsp(ProcessVM *vm, uint64_t v) {
        vm->registers.stack_pointer.raw(v);
    }

    static void write_rflags(ProcessVM *vm, uint64_t v) {
        vm->registers.flags.raw = v;
    }

    static constexpr WriteSpecialFn write_special_table[] = {
        write_cur0,  // 0b000000
        write_cur1,  // 0b000001
        write_cur2,  // 0b000010
        write_cur3,  // 0b000011
        nullptr,     // 0b000100
        nullptr,     // 0b000101
        nullptr,     // 0b000110
        nullptr,     // 0b000111
        write_rip,   // 0b001000
        write_rbp,   // 0b001001
        write_rsp,   // 0b001010
        write_rflags // 0b001011
    };

    inline void write_special(ProcessVM *vm, uint8_t code, uint64_t v) {
        if (code >= sizeof(write_special_table) ||
            write_special_table[code % sizeof(write_special_table)] == nullptr
        ) {
            VM_ASSERT(false, "write_special: code=" + std::to_string(code) + " is not (code >= 12)", {});
            return;
        }
        write_special_table[code](vm, v);
    }



    // ---------------------------------------------------------------------
    //                          de tipo registro
    // ---------------------------------------------------------------------
    /**
     * Permite ejecutar la instruccion INC o DEC dependiendo del valor de
     * instr._signed_instruct:
     *      - (instr._signed_instruct == 0) = INC
     *      - (instr._signed_instruct == 1) = DEC
     * @param vm
     * @param instr
     */
    void exec_instr_inc_dec_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_mov_reg(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion de tipo
     * add reg, reg
     */
    void exec_instr_add_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_sub_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_mul_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_cmp_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_and_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_or_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_xor_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_div_reg(ProcessVM *vm, const DecodedInstr &instr);

    // ---------------------------------------------------------------------


    // ---------------------------------------------------------------------
    //                          de tipo inmediato
    // ---------------------------------------------------------------------
    void exec_instr_add_imm(ProcessVM *vm, const DecodedInstr &instr);

    // ---------------------------------------------------------------------

    /**
     * Permite ejecutar una instruccion HLT, esto hara que la fase
     * EXECUTE emita un evento de tipo EVT_ERROR
     * @param vm
     * @param instr
     */
    void exec_instr_hlt(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion de tipo PUSH.
     * PUSH:
     *      - SP -= size
     *      - [SP] = value
     * @param vm
     * @param instr
     */
    void exec_instr_push(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion de tipo POP.
     * POP:
     *      - value = [SP]
     *      - SP += size
     *
     * @param vm
     * @param instr
     */
    void exec_instr_pop(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion CALLN que realiza una llamada
     * nativa, puede bloquear la VM si la funcion bloquea.
     * @param vm
     * @param instr
     */
    void exec_instr_calln(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_xchg(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion MOV del tipo:
     * mov:
     *      - reg, 0x1000 ||
     *      -  [reg], 0x1000 ||
     *      - reg_ext, 0x1000
     * @param vm
     * @param instr
     */
    void exec_instr_inmed_mov(ProcessVM *vm, const DecodedInstr &instr);

    // -------------------------------------------------------------------------
    //                   GC generacional (0x00 0xA0 .. 0xA3)
    // -------------------------------------------------------------------------

    /** NEWOBJ reg_size -> R0 = GcHandle
     *  reg1 = registro que contiene el numero de bytes a reservar.
     *  Retorna GC_NULL_HANDLE en R0 si no hay memoria disponible. */
    void exec_instr_newobj(ProcessVM *vm, const DecodedInstr &instr);

    /** GCRUN  (sin operandos)
     *  Ejecuta minor_gc() del proceso actual; si old_used >= threshold
     *  tambien ejecuta major_gc(). */
    void exec_instr_gcrun(ProcessVM *vm, const DecodedInstr &instr);

    /** GCCONFIG reg_threshold
     *  reg1 = registro que contiene el nuevo umbral de OldGen (bytes). */
    void exec_instr_gcconfig(ProcessVM *vm, const DecodedInstr &instr);

    /** DROP reg_handle
     *  reg1 = registro que contiene el GcHandle a liberar. */
    void exec_instr_gc_drop(ProcessVM *vm, const DecodedInstr &instr);

    // -------------------------------------------------------------------------
    //                   Raw allocator (0x00 0xB0 .. 0xB2)
    // -------------------------------------------------------------------------

    /** ALLOC reg_size -> R0 = ptr host real (uint64_t)
     *  reg1 = registro que contiene el numero de bytes a reservar.
     *  Retorna 0 en R0 si la asignacion falla. */
    void exec_instr_raw_alloc(ProcessVM *vm, const DecodedInstr &instr);

    /** FREE reg_ptr
     *  reg1 = registro que contiene el puntero host a liberar.
     *  No hace nada si el puntero no pertenece a este proceso. */
    void exec_instr_raw_free(ProcessVM *vm, const DecodedInstr &instr);

    /** REALLOC reg_ptr, reg_size -> R0 = nuevo ptr host real
     *  reg1 = registro con el puntero original.
     *  reg2 = registro que contiene el nuevo tamano en bytes.
     *  Retorna 0 en R0 si la asignacion falla. */
    void exec_instr_raw_realloc(ProcessVM *vm, const DecodedInstr &instr);
}
#endif //EXEC_INSTRUCTION_H
