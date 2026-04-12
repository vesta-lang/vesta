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

    using ReadRegFn = uint64_t(*)(VM *, uint8_t);

    static uint64_t read_reg8(VM *vm, uint8_t r) {
        return vm->regs[r].byte_lo();
    }

    static uint64_t read_reg16(VM *vm, uint8_t r) {
        return vm->regs[r].word_lo();
    }

    static uint64_t read_reg32(VM *vm, uint8_t r) {
        return vm->regs[r].dword_lo();
    }

    static uint64_t read_reg64(VM *vm, uint8_t r) {
        return vm->regs[r].qword();
    }

    static constexpr ReadRegFn read_reg_table[] = {
        read_reg8,
        read_reg16,
        read_reg32,
        read_reg64
    };

    using WriteRegFn = void(*)(VM *, uint8_t, uint64_t);

    static void write_reg8(VM *vm, uint8_t r, uint64_t v) {
        vm->regs[r].byte_lo(v);
    }

    static void write_reg16(VM *vm, uint8_t r, uint64_t v) {
        vm->regs[r].word_lo(v);
    }

    static void write_reg32(VM *vm, uint8_t r, uint64_t v) {
        vm->regs[r].dword_lo(v);
    }

    static void write_reg64(VM *vm, uint8_t r, uint64_t v) {
        vm->regs[r].qword(v);
    }

    static constexpr WriteRegFn write_reg_table[] = {
        write_reg8,
        write_reg16,
        write_reg32,
        write_reg64
    };

    using ReadSpecialFn = uint64_t(*)(VM *);

    static uint64_t read_cur0(VM *vm) {
        return vm->cur[0].qword();
    }

    static uint64_t read_cur1(VM *vm) {
        return vm->cur[1].qword();
    }

    static uint64_t read_cur2(VM *vm) {
        return vm->cur[2].qword();
    }

    static uint64_t read_cur3(VM *vm) {
        return vm->cur[3].qword();
    }

    static uint64_t read_rip(VM *vm) {
        return vm->rip.raw();
    }

    static uint64_t read_rbp(VM *vm) {
        return vm->base_pointer.raw();
    }

    static uint64_t read_rsp(VM *vm) {
        return vm->stack_pointer.raw();
    }

    static uint64_t read_rflags(VM *vm) {
        return vm->flags.raw;
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


    inline uint64_t read_special(VM *vm, uint8_t code) {
        if (code >= 12 || read_special_table[code] == nullptr) {
            vm->should_kill = true;
            return 0;
        }
        return read_special_table[code](vm);
    }

    using WriteSpecialFn = void(*)(VM *, uint64_t);

    static void write_cur0(VM *vm, uint64_t v) {
        vm->cur[0].qword(v);
    }

    static void write_cur1(VM *vm, uint64_t v) {
        vm->cur[1].qword(v);
    }

    static void write_cur2(VM *vm, uint64_t v) {
        vm->cur[2].qword(v);
    }

    static void write_cur3(VM *vm, uint64_t v) {
        vm->cur[3].qword(v);
    }

    static void write_rip(VM *vm, uint64_t v) {
        vm->rip.raw(v);
        // al modificar rip, se esta haciendo un salto
        vm->decoded_ptr->flags_info.did_jump = true;
    }

    static void write_rbp(VM *vm, uint64_t v) {
        vm->base_pointer.raw(v);
    }

    static void write_rsp(VM *vm, uint64_t v) {
        vm->stack_pointer.raw(v);
    }

    static void write_rflags(VM *vm, uint64_t v) {
        vm->flags.raw = v;
    }

    static constexpr WriteSpecialFn write_special_table[12] = {
        write_cur0,  // 0
        write_cur1,  // 1
        write_cur2,  // 2
        write_cur3,  // 3
        nullptr,     // 4
        nullptr,     // 5
        nullptr,     // 6
        nullptr,     // 7
        write_rip,   // 8
        write_rbp,   // 9
        write_rsp,   // 10
        write_rflags // 11
    };

    inline void write_special(VM *vm, uint8_t code, uint64_t v) {
        if (code >= 12 || write_special_table[code] == nullptr) {
            vm->should_kill = true;
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
    void exec_instr_inc_dec_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_mov_reg(VM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion de tipo
     * add reg, reg
     */
    void exec_instr_add_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_sub_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_mul_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_cmp_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_and_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_or_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_xor_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_div_reg(VM *vm, const DecodedInstr &instr);

    // ---------------------------------------------------------------------


    // ---------------------------------------------------------------------
    //                          de tipo inmediato
    // ---------------------------------------------------------------------
    void exec_instr_add_imm(VM *vm, const DecodedInstr &instr);

    // ---------------------------------------------------------------------

    /**
     * Permite ejecutar una instruccion HLT, esto hara que la fase
     * EXECUTE emita un evento de tipo EVT_ERROR
     * @param vm
     * @param instr
     */
    void exec_instr_hlt(VM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion de tipo PUSH.
     * PUSH:
     *      - SP -= size
     *      - [SP] = value
     * @param vm
     * @param instr
     */
    void exec_instr_push(VM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion de tipo POP.
     * POP:
     *      - value = [SP]
     *      - SP += size
     *
     * @param vm
     * @param instr
     */
    void exec_instr_pop(VM *vm, const DecodedInstr &instr);
}
#endif //EXEC_INSTRUCTION_H
