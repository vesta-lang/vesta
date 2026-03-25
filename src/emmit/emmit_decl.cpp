#include "emmit/emmit_decl.h"

#include "ftxui/component/event.hpp"

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
    uint8_t emmit_reg(const char *const name) {
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

    void emit_inc(
        const vm::Instruction *instruction_parser,
        std::vector<uint8_t> & code_final,
        const InstrInfo *      now_instr
    ) {

        // si el opcode es un dec, entonces, el segundo byte tiene el bit dos activo
        uint8_t reg = (instruction_parser->opcode == "inc") ? 0 : 0b01000000;

        // INC solo usa un operando de tipo registro
        if (auto s = dynamic_cast<vm::RegisterOperand *>(instruction_parser->operands[0].get())) {
            reg += emmit_reg(s->name.c_str()); // codificamos el registro
            code_final.push_back(reg);
        } else {
            throw std::runtime_error("Registro inválido: esta instruccion INC espera un registro");
        }
        DEBUG_PRINT("Emitiendo INC 0x%x REG: 0x%x, Size: %llu\n",
                    now_instr->opcode1, reg,
                    instr_size(now_instr->sizeMode)
        );
    }
}
