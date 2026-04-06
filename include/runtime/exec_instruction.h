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
        0xFFULL, // 8 bits
        0xFFFFULL, // 16 bits
        0xFFFFFFFFULL, // 32 bits
        0xFFFFFFFFFFFFFFFFULL // 64 bits
    };

    static constexpr int SIGNBIT[4] = {7, 15, 31, 63};

    void exec_instr_inc_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_dec_reg(VM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion de tipo
     * add reg, reg
     */
    void exec_instr_add_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_sub_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_cmp_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_and_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_or_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_xor_reg(VM *vm, const DecodedInstr &instr);

    void exec_instr_div_reg(VM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion HLT, esto hara que la fase
     * EXECUTE emita un evento de tipo EVT_ERROR
     * @param vm
     * @param instr
     */
    void exec_instr_hlt(VM *vm, const DecodedInstr &instr);
}
#endif //EXEC_INSTRUCTION_H
