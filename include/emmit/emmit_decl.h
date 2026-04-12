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

#include "bytewriter.h"
#include "runtime/vm_address_space.h" // necesitamos los espacios de direciones por defecto
#include "parser/parser.h"

namespace Assembly::Bytecode {
    class Assembler;
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

        /**
         * La instruccion siempre ocupa 10 bytes
         */
        FIXED_10,

        COUNT,

        /**
         * La instruccion puede tener un tamaño variable como son las instrucciones
         * que usan valores inmediatos. estos suelen variar entre:
         *
         * @code
         *  //  Total       opcode      inmediato
         *  //    4    (    3           + 1),
         *  //    5    (    3           + 2)
         *  //    7    (    3           + 4)
         *  //    11   (    3           + 8)
         *  //  bytes
         * @endcode
         */
        MIXED_SIZE
    };

    /**
     * Se usa para indicar el tamaño del inmediato y ademas si es con o sin signo.
     */
    enum class ImmType {
        U8, S8,
        U16, S16,
        U32, S32,
        U64, S64
    };

    /**
     * Codificacion para registros extendidos o especiales.
     */
    static const std::unordered_map<std::string, uint8_t> special_reg_encoding = {
        {"cur0", 0b000000},
        {"cur1", 0b000001},
        {"cur2", 0b000010},
        {"cur3", 0b000011},

        {"rip", 0b001000},
        {"rbp", 0b001001},
        {"rsp", 0b001010},
        {"rflags", 0b001011},
    };

    /**
     * Permite obtener la codificacion de un registro especial/extendido, en caso de no ser uno de estos,
     * podria ser un registro general u otra cosa.
     * @param reg supuesto registro especial.
     * @return codificacion del registro, en caso de no ser un registro especial, devuelve std::nullopt.
     */
    inline std::optional<uint8_t> encode_special_register(const std::string &reg) {
        auto it = special_reg_encoding.find(reg);
        if (it != special_reg_encoding.end())
            return it->second;

        return std::nullopt;
    }


    /**
     * Permite obtener el tamaño de un valor y si es con o sin signo
     * @param value valor a comprobar
     * @param is_signed si tiene o no tiene signo
     * @return devuelve la variante que es.
     */
    inline ImmType detect_imm_type(int64_t value, bool is_signed) {
        if (is_signed) {
            if ((int8_t) value == value) return ImmType::S8;
            if ((int16_t) value == value) return ImmType::S16;
            if ((int32_t) value == value) return ImmType::S32;
            return ImmType::S64;
        } else {
            uint64_t v = (uint64_t) value;
            if ((uint8_t) v == v) return ImmType::U8;
            if ((uint16_t) v == v) return ImmType::U16;
            if ((uint32_t) v == v) return ImmType::U32;
            return ImmType::U64;
        }
    }

    inline int immtype_bits(ImmType t) {
        switch (t) {
            case ImmType::U8:
            case ImmType::S8: return 8;
            case ImmType::U16:
            case ImmType::S16: return 16;
            case ImmType::U32:
            case ImmType::S32: return 32;
            default: return 64;
        }
    }

    /**
     * modo 0(00) -> (0000 1000) 8  bits
     * modo 1(01) -> (0001 0000) 16 bits
     * modo 2(10) -> (0010 0000) 32 bits
     * modo 3(11) -> (0100 0000) 64 bits
     * @param mode modo a codear
     * @return tamaño en bits
     */
    inline int mode_to_bits(uint8_t mode) {
        return 1 << (3 + mode);
    }

    /**
     * modo 0(00) -> (0000 0001) 1 bytes
     * modo 1(01) -> (0000 0010) 2 bytes
     * modo 2(10) -> (0000 0100) 4 bytes
     * modo 3(11) -> (0000 1000) 8 bytes
     * @param mode modo a codear
     * @return tamaño en bits
     */
    inline int mode_to_bytes(uint8_t mode) {
        return 1 << mode;
    }

    /**
     * Permie codificar el modo del registro, 0, 1, 2, 3
     * 0(0b00) = 1(0b1) byte   = 8 bits
     * 1(0b01) = 2(0b10) bytes  = 16 bits
     * 2(0b10) = 4(0b100) bytes  = 32 bits
     * 3(0b11) = 8(0b1000) bytes  = 64 bits
     * @param size_bits
     * @return
     */
    inline uint8_t encode_mode(int size_bits) {
        // si da un byte, al desplazar se queda en 0
        // si da dos bytes, al desplazar queda en 1
        // si da 4 bytes, al deslazar queda en 2
        // si da 8 bytes, al desplazar queda en 4
        uint8_t n_mode = (size_bits / 8) >> 1;
        return (n_mode >= 4) ? 3 : n_mode;
    }

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
        NONE,

        /**
         * Solo se usa para contar la cantidad
         */
        COUNT
    };

    static std::string AddressingMode_str(AddressingMode mode) {
        static const std::string table[(size_t) AddressingMode::COUNT] = {"REG", "MEM", "SIB", "INMED", "NONE"};
        return table[static_cast<uint8_t>(mode)];
    }

    /**
     * Permite obtener el tamaño de una instruccion
     * @param mode modo o tamaño tambien llamado en algunos casos.
     * @return tamaño de la instruccion.
     */
    constexpr size_t instr_size(InstrSizeMode mode) {
        constexpr size_t table[(size_t) InstrSizeMode::COUNT] = {1, 2, 4, 8, 10};
        return table[static_cast<uint8_t>(mode)];
    }

    /**
    * Tipo puntero a funcion para emitir la instruccion
    */
    typedef void (*emitInstr)(
        const vm::Instruction *,  // instruccion analizar por el parser
        ByteWriter &,             // lugar donde emitir la instruccion
        const struct InstrInfo *, // la propia estructura que contiene la informacion de la
        // instruccion codificada
        Assembler *assembly_ctx
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
    static bool is_signed(const std::string &opcode) {
        return signed_ops.count(opcode) > 0;
    }

    void emit_pop_push(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    void emit_inc_dec(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    void emit_instr_reg(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    void emit_instr_mem(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    void emit_instr_sib(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * Permite emitir isntrucciones de tipo inmediato como son MOV r12, 0x1234 y otras similares
     * @param instruction_parser
     * @param code_final
     * @param now_instr
     * @param assembly_ctx
     */
    void emit_instr_inmed(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * Permite emitir instrucciones inmediatas donde el segundo operando es una label o una notacion,
     * este tipo de instrucciones requiere crear una entrada en la tabla de relocalizaciones que el
     * linker en run time debera resolver. Este metodo nunca es llamado directamente, sino que lo hara
     * `emit_instr_inmed` cuando detecte que no es una instruccion convencional de inmediatos.
     * @param instruction_parser
     * @param code_final
     * @param now_instr
     * @param assembly_ctx
     */
    void emit_instr_inmed_with_annotation(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );
}
#endif //EMMIT_DECL_H
