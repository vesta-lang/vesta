#include "emmit/emmit_decl.h"

#include "emmit/parser_to_bytecode.h"

#define DEBUG_EMIT

#ifdef DEBUG_EMIT
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...) if (0) {printf(__VA_ARGS__)}
#endif


/**
 * La isntruccion de emision emiten el resto de bytes faltantes,
 * el opcode/s ya debio ser emitido por a funcion llamadora del metodo de emision.
 */
namespace Assembly::Bytecode {
    /**
     * Permite codificar el registro unicamente, independientemente del "modo"/tamaño de este,
     * ya que r00, r00d, r00w y r00b usando el mismo valor para coficarse, 0, pero cambia el modo.
     *
     * @param name nombre del registro a codear
     * @return registro coficado, si es de proposito general, no se incluye el modo
     */
    uint8_t encode_reg(const char *const name) {
        uint8_t n_reg = 0;

        if (name[0] == 'r' || name[0] == 'R') {
            // si despues de la r va un digito, entonces puede ser una instruccion
            // con un solo digito
            if (name[1] && isdigit(static_cast<unsigned char>(name[1]))) {
                n_reg = name[1] - '0'; // obtenmos el primer digito

                // en este caso de que haya segundo digito, se analiza
                if (name[2] && isdigit(static_cast<unsigned char>(name[2]))) {
                    // evitamos si codifican el registro como r01
                    if (n_reg != 0) {
                        // obtenmos el segundo digito y multiplicamos al anterior
                        // para obtener el valor final
                        return (n_reg * 10) + (name[2] - '0');
                    }

                    // al ser r01, retornamo el segundo digito unicamente
                    return (name[2] - '0');
                }
                return n_reg;
            }
            throw std::runtime_error("Registro inválido: falta número tras 'r'");
        }
        throw std::runtime_error("Registro inválido: debe comenzar por 'r'");
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
    uint8_t encode_mode(int size_bits) {
        // si da un byte, al desplazar se queda en 0
        // si da dos bytes, al desplazar queda en 1
        // si da 4 bytes, al deslazar queda en 2
        // si da 8 bytes, al desplazar queda en 4
        uint8_t n_mode = (size_bits / 8) >> 1;
        return (n_mode >= 4) ? 3 : n_mode;
    }

    void emit_inc(
        const vm::Instruction *instruction_parser,
        ByteWriter &code_final,
        const InstrInfo *now_instr,
        Assembler* assembly_ctx
    ) {
        // si el opcode es un dec, entonces, el segundo byte tiene el bit dos activo
        uint8_t reg = (instruction_parser->opcode == "inc") ? 0 : 0b01000000;

        // INC solo usa un operando de tipo registro
        if (auto s = dynamic_cast<vm::RegisterOperand *>(instruction_parser->operands[0].get())) {
            reg += encode_reg(s->name.c_str()); // codificamos el registro
            code_final.emit8(reg);
        } else {
            throw std::runtime_error("Registro inválido: esta instruccion INC espera un registro");
        }
        DEBUG_PRINT("Emitiendo INC 0x%x REG: 0x%x, Size: %llu\n",
                    now_instr->opcode1, reg,
                    instr_size(now_instr->sizeMode)
        );
    }

    void emit_instr_reg(
        const vm::Instruction *instruction_parser,
        ByteWriter &code_final,
        const InstrInfo *now_instr,
        Assembler* assembly_ctx
    ) {
        auto reg1 = dynamic_cast<vm::RegisterOperand *>(instruction_parser->operands[0].get());
        auto reg2 = dynamic_cast<vm::RegisterOperand *>(instruction_parser->operands[1].get());

        if (reg1 == nullptr || reg2 == nullptr) {
            throw std::runtime_error(
                "Error, la instruccion " + instruction_parser->opcode + " esperaba dos registros"
            );
        }

        if (reg1->size_bits != reg2->size_bits) {
            throw std::runtime_error(
                "Error, la instruccion " + instruction_parser->opcode + " " +
                reg1->name + " " + reg2->name + " tiene sizes distintos en los " +
                "registros, deben tener el mismo size."
            );
        }
        uint8_t mode = encode_mode(reg1->size_bits);

        // 0b`mode`0d0000 -> modo ocupa el los primeros 2 bits
        code_final.emit8(mode << 6);

        // los dos registros se codifica en el mismo byte (en el cuarto normalmente), el modo en el tercero
        code_final.emit8(
            encode_reg(reg2->name.c_str()) << 4 | encode_reg(reg1->name.c_str())
        );

        DEBUG_PRINT("Emitiendo %s 0x%02x 0x%02x REG1(%s): 0x%02x, REG2(%s): 0x%02x MODE: %d\n",
                    instruction_parser->opcode.c_str(),
                    now_instr->opcode1,
                    now_instr->opcode2,
                    reg1->name.c_str(), encode_reg(reg2->name.c_str()) << 4,
                    reg2->name.c_str(), encode_reg(reg1->name.c_str()),
                    mode
        );
    }

    void emit_instr_mem(
        const vm::Instruction *instruction_parser,
        ByteWriter &code_final,
        const InstrInfo *now_instr,
        Assembler* assembly_ctx
    ) {
        if (now_instr->opcode1 != 0x00) {
            throw std::runtime_error("Error instruccion instruccion != 0x00 no implementada");
        }
        bool is_a_signed = is_signed(instruction_parser->opcode);

        auto n0 = instruction_parser->operands[0].get();
        auto n1 = instruction_parser->operands[1].get();

        uint8_t direccion = 1; // [mem], reg
        auto mem = dynamic_cast<vm::MemoryOperand *>(n0);
        auto reg = dynamic_cast<vm::RegisterOperand *>(n1);

        // si el op1 no era de memoria o el op2 no era de tipo registro, probar al reves:
        if (mem == nullptr || reg == nullptr) {
            direccion = 0; // reg, [mem]
            mem = dynamic_cast<vm::MemoryOperand *>(n1);
            reg = dynamic_cast<vm::RegisterOperand *>(n0);
        }

        if (mem == nullptr || reg == nullptr) {
            throw std::runtime_error("Error instruccion instruccion: " + instruction_parser->opcode +
                                     " no pudo situar un operando de tipo memoria, mas de otro operando de tipo registro,");
        }
        uint8_t mode = encode_mode(reg->size_bits);

        /**
         * mode       = 01 << 6 = 0100 0000
         * signed     = 1  << 5 = 0010 0000
         * direccion  = 0  << 4 = 0000 0000
         * reg (7)    = 0000 0111
         *
         * Resultado:   0110 0111   = 0x67
         * [ mode(2) | signed(1) | direccion(1) | reg(4) ]
         */
        uint8_t byte = (mode << 6) |
                       (is_a_signed << 5) |
                       (direccion << 4) |
                       encode_reg(reg->name.c_str());
        code_final.emit8(byte);

        // por ahora supondremos que solo se introdujo operando de memoria de un solo label tipo [my_label]
        auto lalbel = dynamic_cast<vm::LabelOperand *>(mem->expr.get());

        Relocation rel;
        rel.symbol  = lalbel->name;
        rel.section = assembly_ctx->current_section->name;
        rel.offset  = code_final.offset /*- 5*/; // el -5 se pone si se emite antes la direccion
        rel.type    = Relocation::Type::Relative40;

        assembly_ctx->ctx.add_relocation(rel);

        // esto es un placeholder que luego el linker debe corregir realizando una relocalizacion.
        code_final.emit40(0x1122334455);
    }

    void emit_instr_sib(
        const vm::Instruction *instruction_parser,
        ByteWriter &code_final,
        const InstrInfo *now_instr,
        Assembler* assembly_ctx
    ) {
        bool is_a_signed = is_signed(instruction_parser->opcode);
    }
}
