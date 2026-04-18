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

#include "emmit/parser_to_bytecode.h"

namespace Assembly::Bytecode {
    size_t Assembler::size_of_directive(const std::string &dir) const {
        if (dir == "db") return 1;
        if (dir == "dw") return 2;
        if (dir == "dd") return 4;
        if (dir == "dq") return 8;

        // Puntero del tamaño de la VM (64 bits)
        if (dir == "ptr") return sizeof(uint64_t);

        throw std::runtime_error("Unknown data directive: " + dir);
    }

    /**
     * 1 Recibir el valor final (ya evaluado)
     * 2 Escribirlo en little-endian
     * 3 Usar el tamaño correcto según la directiva
     *
     * @param dir
     * @param value
     */
    void Assembler::emit_directive(const std::string &dir, uint64_t value) {
        size_t size = size_of_directive(dir);

        for (size_t i = 0; i < size; ++i) {
            output.emit8(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
        }
    }

    /**
     * Las cadenas deben emitirse caracter a caracter
     * @param data datos de la cadena
     */
    void Assembler::emit_data(const vm::DataDecl *data) {
        for (auto &expr: data->values) {
            if (auto s = dynamic_cast<vm::StringExpr *>(expr.get())) {
                for (char c: s->value)
                    output.emit8(static_cast<uint8_t>(c));
                continue;
            }

            // Para números, labels, expresiones:
            uint64_t v = eval_expr(expr.get());
            emit_directive(data->directive, v);
        }
    }

    const InstrInfo &Assembler::select_variant(
        const std::string &                               mnemonic,
        const std::vector<std::unique_ptr<vm::ASTNode> > &ops) const {
        auto it = InstrTable.find(mnemonic);
        if (it == InstrTable.end())
            throw std::runtime_error("select_variant(): Unknown instruction in InstrTable: " + mnemonic);

        const auto &variants = it->second;

        // si es un registro el primero
        AddressingMode mode = AddressingMode::NONE;

        // si no tiene operandos, suponemos que es una instruccion sin tal
        if (ops.size() == 0) goto search_variante_None;

        if (auto s = dynamic_cast<vm::RegisterOperand *>(ops[0].get())) {
            mode = AddressingMode::REG;
        }

        // si hay mas de dos operandos, entonces, el primer operando si es un registro
        // no se puede usar para averiguar el direccionamiento de la isntruccion.
        if (ops.size() >= 2) {
            // si el segundo operando es de tipo memoria, el modo de direccionamiento es este u SIB
            if (auto s = dynamic_cast<vm::MemoryOperand *>(ops[1].get())) {
                mode = AddressingMode::MEM;

                if (auto bin = dynamic_cast<vm::BinaryExpr *>(s->expr.get())) {
                    if (bin->op == '-' || bin->op == '+' || bin->op == '*') {
                        mode = AddressingMode::SIB;
                    }
                }
            }

            // si el op2 es un registro, el modo de direcionamiento confirmado es registro
            else if (auto s = dynamic_cast<vm::RegisterOperand *>(ops[1].get())) {
                mode = AddressingMode::REG;
            } else if (auto s = dynamic_cast<vm::AnnotationNode *>(ops[1].get())) {
                // si el segundo operando es una notacion
                if (s->key == "Method" || s->key == "Relative" || s->key == "Absolute") {
                    // si el segundo operando es una notacion Method
                    // entonces el modo de operacion es de tipo INMEDIATO, y se esta pidiendo indica la direccion
                    // de memoria de un metodo que debe haber sido cargado por el loader-linker en run time.
                    mode = AddressingMode::INMED;
                } else {
                    throw std::runtime_error(
                        "select_variant(): No se permite usar esta notacion (" + s->key + ") en la instruccion: " +
                        mnemonic);
                }
            }

            // es de tipo inmed [0x1000]
            else if (auto s = dynamic_cast<vm::NumberOperand *>(ops[1].get())) {
                mode = AddressingMode::INMED;
            }

            // si el operando 1 es de tipo memoria, el resto de operandos da igual de que tipo sea,
            // ya que siempre sera memoria. por eso usar if y no else if aqui
            if (auto s = dynamic_cast<vm::MemoryOperand *>(ops[0].get())) {
                mode = AddressingMode::MEM;

                if (auto bin = dynamic_cast<vm::BinaryExpr *>(s->expr.get())) {
                    if (bin->op == '-' || bin->op == '+' || bin->op == '*') {
                        mode = AddressingMode::SIB;
                    }
                }
            }

            // error?
        } else {
            // si solo hay un operando, y fue un registro, entonces, es correcto

            // si no hubo registro, y solo hay un operando, debe ser un inmediato
            if (auto s = dynamic_cast<vm::NumberOperand *>(ops[0].get())) {
                mode = AddressingMode::INMED;
            } else if (auto s = dynamic_cast<vm::AnnotationNode *>(ops[0].get())) {
                // si el segundo operando es una notacion
                if (s->key == "Method" || s->key == "Relative" || s->key == "Absolute") {
                    // si el segundo operando es una notacion Method
                    // entonces el modo de operacion es de tipo INMEDIATO, y se esta pidiendo indica la direccion
                    // de memoria de un metodo que debe haber sido cargado por el loader-linker en run time.
                    mode = AddressingMode::INMED;
                } else {
                    throw std::runtime_error(
                        "select_variant(): No se permite usar esta notacion (" + s->key + ") en la instruccion: " +
                        mnemonic);
                }
            }
        }

        // si este caso se da, quiere decir que el operando usa un inmediato que se mueve a memoria, por ejemplo:
        // adds [r0], 0x1000
        if (mode == AddressingMode::MEM) {
            if (auto s = dynamic_cast<vm::NumberOperand *>(ops[1].get())) {
                mode = AddressingMode::INMED;
            }
        }

        // alguna instrucciones como los NOP no tiene operandos,
        // por lo que todoo el analisis anterior se puede saltar
    search_variante_None:
        int idx = -1;
        for (int i = 0; i < variants.size(); ++i) {
            if (variants[i].mode == mode) {
                idx = i; // indice obtenido, sabemos cual es la variante
                break;
            }
        }

        if (idx != -1) {
            return variants[idx]; // devolvemos la variante
        }

        throw std::runtime_error("select_variant(): Unknown instruction variants: " + mnemonic);
    }


    void Assembler::emit_instruction(const vm::Instruction *instr) {
        const auto &info = select_variant(instr->opcode, instr->operands);

        // 1 opcode1
        output.emit8(info.opcode1);

        // 2 si opcode1 == 0x00, emito opcode2
        if (info.opcode1 == 0x00)
            output.emit8(info.opcode2);

        // 3 resto de campos según la instrucción (flags, reg, SIB, addr, etc.)
        //    aquí ya mirar los operandos y generar el encoding real.
        if (info.emit != nullptr) {
            info.emit(instr, output, &info, this);
        } else {
            std::cout << "La isntruccion: " << instr->opcode <<
                    " no esta implementada en el ensamblador, no tiene una unidad de emision" << std::endl;
        }
    }
}
