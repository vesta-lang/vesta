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

    /**
     * Modos de direccionamiento de la instruccion
     */
    enum class AddressingMode {
        /**
         * Permtie codificar instrucciones y variantes que usan unicamente un registro o varios,
         * en caso de ser varios, suelen permitir la codifacion del tipo "reg1, reg2" y "reg2, reg1"
         * con un bit que indica la direccionalidad de la operacion.
         */
        REG,

        /**
         * Permite operaciones unicamente con memoria, o de registro a memoria y al reves:
         *      adds reg1, [mem]
         *      adds [mem], reg1
         *  normalmente un campo indica la direccionalidad de la operacion
         */
        MEM,

        /**
         * Permite operaciones o variantes que usan sib con memoria o registros y al reves:
         *      adds reg, [reg1 * reg2 + reg3]
         */
        SIB,

        /**
         * Se usa para indicar que la isntruccion usa inmediatos o es una variante que lo hace. Una direccion de memoria
         * constante o un valor se considera inmediato:
         *      adds r00, 0x1234
         */
        INMED,

        /**
         * Sin modos de direccionamiento
         */
        NONE
    };

    static std::string AddressingMode_str(AddressingMode mode) {
        static const std::string table[] = {"REG", "MEM", "SIB", "INMED", "NONE"};
        return table[static_cast<uint8_t>(mode)];
    }

    constexpr size_t instr_size(InstrSizeMode mode) {
        constexpr size_t table[] = {};
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

        AddressingMode mode;

        emitInstr emit;
    } InstrInfo;

    static const std::unordered_set<std::string> signed_ops = {
        "adds", "subs", "muls", "divs", "cmps"
    };

    /**
     * Coste O(1) para busqueda de isntrucciones con signo
     * @param opcode nombre de la isntruccion a comprobar si es con signo
     * @return true si fue una instruccion contemplada con signo.
     */
    static bool is_signed(const std::string& opcode) {
        return signed_ops.count(opcode) > 0;
    }

    void emit_inc(
        const vm::Instruction *instruction_parser,
        std::vector<uint8_t> & code_final,
        const InstrInfo *      now_instr
    );

    void emit_instr_reg(
        const vm::Instruction *instruction_parser,
        std::vector<uint8_t> & code_final,
        const InstrInfo *      now_instr
    );

    void emit_instr_mem(
        const vm::Instruction *instruction_parser,
        std::vector<uint8_t> & code_final,
        const InstrInfo *      now_instr
    );

    void emit_instr_sib(
        const vm::Instruction *instruction_parser,
        std::vector<uint8_t> & code_final,
        const InstrInfo *      now_instr
    );
}
#endif //EMMIT_DECL_H
