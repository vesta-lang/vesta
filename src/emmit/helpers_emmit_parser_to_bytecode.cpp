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
    size_t Assembler::size_of_directive(const std::string& dir) const {
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
    void Assembler::emit_directive(const std::string& dir, uint64_t value) {
        size_t size = size_of_directive(dir);

        for (size_t i = 0; i < size; ++i) {
            output.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
        }

        current_offset += size;
    }

    /**
     * Las cadenas deben emitirse caracter a caracter
     * @param data datos de la cadena
     */
    void Assembler::emit_data(const vm::DataDecl* data) {
        for (auto& expr : data->values) {

            if (auto s = dynamic_cast<vm::StringExpr*>(expr.get())) {
                for (char c : s->value)
                    output.push_back(static_cast<uint8_t>(c));

                current_offset += s->value.size();
                continue;
            }

            // Para números, labels, expresiones:
            uint64_t v = eval_expr(expr.get());
            emit_directive(data->directive, v);
        }
    }

    const InstrInfo& Assembler::select_variant(
        const std::string&                              mnemonic,
        const std::vector<std::unique_ptr<vm::ASTNode>>&ops)
    {
        auto it = InstrTable.find(mnemonic);
        if (it == InstrTable.end())
            throw std::runtime_error("Unknown instruction: " + mnemonic);

        const auto& variants = it->second;

        // De momento, como sizeMode es FIXED_x en todas,
        // puedes devolver directamente la primera variante.
        return variants.front();
    }

    size_t Assembler::size_of_instruction(const vm::Instruction* instr) const {
        const auto& info = select_variant(instr->opcode, instr->operands);

        switch (info.sizeMode) {
            case InstrSizeMode::FIXED_1: return 1;
            case InstrSizeMode::FIXED_2: return 2;
            case InstrSizeMode::FIXED_4: return 4;
            case InstrSizeMode::FIXED_8: return 8;
        }
        throw std::runtime_error("Invalid size mode");
    }

    void Assembler::emit_instruction(const vm::Instruction *instr) {

        const auto& info = select_variant(instr->opcode, instr->operands);

        // 1 opcode1
        output.push_back(info.opcode1);

        // 2 si opcode1 == 0x00, emito opcode2
        if (info.opcode1 == 0x00)
            output.push_back(info.opcode2);

        // 3 resto de campos según la instrucción (flags, reg, SIB, addr, etc.)
        //    aquí ya mirar los operandos y generar el encoding real.
        if (info.emit != nullptr) {
            info.emit(instr, output, &info);
        }
    }



}