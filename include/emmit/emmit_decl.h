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
#ifndef EMMIT_DECL_H
#define EMMIT_DECL_H
#include <unordered_set>

#include "runtime/vm_address_space.h" // necesitamos los espacios de direciones por defecto
#include "parser/parser.h"

namespace Assembly::Bytecode {
    /**
     * Metadatos de la instruccion
     */
    enum class InstrSizeMode {
        /**
         * La instrucción siempre ocupa 1 byte.
         */
        FIXED_1,

        /**
         * La instrucción siempre ocupa 2 bytes.
         */
        FIXED_2,

        /**
         * La instrucción siempre ocupa 4 bytes.
         */
        FIXED_4,

        /**
         * La instrucción siempre ocupa 8 bytes.
         */
        FIXED_8,

    };

    constexpr size_t instr_size(InstrSizeMode mode) {
        constexpr size_t table[] = {1, 2, 4, 8};
        return table[static_cast<uint8_t>(mode)];
    }

    /**
    * Tipo puntero a funcion para emitir la instruccion
    */
    typedef void (*emitInstr)(
        const vm::Instruction *, // instruccion analizar por el parser
        std::vector<uint8_t> &,  // lugar donde emitir la instruccion
        const struct InstrInfo * // la propia estructura que contiene la informacion de la
        // instruccion codificada
    );

    typedef struct InstrInfo {
        /**
         * En caso de este campo ser 0, se usa como extension de operacion
         */
        uint8_t opcode1;
        uint8_t opcode2; // solo se usa si opcode1 = 0x0

        InstrSizeMode sizeMode;

        emitInstr emit;
    } InstrInfo;

    void emit_inc(
        const vm::Instruction *instruction_parser,
        std::vector<uint8_t> & code_final,
        const InstrInfo *      now_instr
    );
}
#endif //EMMIT_DECL_H
