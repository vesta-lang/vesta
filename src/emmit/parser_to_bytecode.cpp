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
    uint64_t Assembler::eval_operand(const vm::ASTNode *op) {
        // número inmediato como operando
        if (auto num = dynamic_cast<const vm::NumberOperand *>(op)) {
            return vm::parse_number(num->value);
        }

        // label como operando
        if (auto lab = dynamic_cast<const vm::LabelOperand *>(op)) {
            auto it = symbol_table.find(lab->name);
            if (it == symbol_table.end())
                throw std::runtime_error("Label no definido: " + lab->name);
            return it->second;
        }

        // si algún día permitir expresiones como operando:
        if (auto expr = dynamic_cast<const vm::ExprNode *>(op)) {
            return eval_expr(const_cast<vm::ExprNode *>(expr));
        }

        //throw std::runtime_error("Operando no valido para directiva (se esperaba numero o label)");
        return 0;
    }

    uint64_t Assembler::eval_expr(vm::ExprNode *expr) {
        if (auto n = dynamic_cast<vm::NumberExpr *>(expr))
            return vm::parse_number(n->value);

        if (auto l = dynamic_cast<vm::LabelExpr *>(expr))
            return symbol_table[l->name];

        if (auto b = dynamic_cast<vm::BinaryExpr *>(expr)) {
            uint64_t L = eval_expr(b->left.get());
            uint64_t R = eval_expr(b->right.get());
            switch (b->op) {
                case '+': return L + R;
                case '-': return L - R;
                case '*': return L * R;
                case '/': return L / R;
            }
        }

        throw std::runtime_error("Invalid expression");
    }

    Assembler::Assembler()
        : default_address(vm::mem::CODE_BEGIN),
          current_offset(0) // dirección base
    {
        output.clear();
        symbol_table.clear();
    }

    std::vector<uint8_t> Assembler::assemble(
        const std::vector<std::unique_ptr<vm::ASTNode> > &ast) {
        uint64_t offset = 0;

        // 1 Primera pasada
        for (auto &node: ast)
            first_pass(node.get(), offset);

        current_offset = 0;
        for (auto &node : ast)
            emit_pass(node.get());


        // 2 Segunda pasada (datos)
        /*for (auto &node: ast)
            second_pass_data(node.get());

        current_offset = 0;

        // 3 Tercera pasada (código)
        for (auto &node: ast)
            third_pass_code(node.get());*/

        return output;
    }


    void Assembler::emit_pass(const vm::ASTNode *node) {
        if (auto sec = dynamic_cast<const vm::SectionNode*>(node)) {
            for (auto &child : sec->body)
                emit_pass(child.get());
        }
        else if (auto data = dynamic_cast<const vm::DataDecl*>(node)) {
            emit_data(data);
        }
        else if (auto instr = dynamic_cast<const vm::Instruction*>(node)) {
            if (PseudoInstructions.count(instr->opcode)) {
                apply_directive(instr);
            } else {
                emit_instruction(instr);
            }
        }
    }


    void Assembler::first_pass(const vm::ASTNode *node, uint64_t &offset) {
        // --- SECCIONES ---
        if (auto sec = dynamic_cast<const vm::SectionNode *>(node)) {
            symbol_table[sec->name] = offset;

            for (auto &child: sec->body)
                first_pass(child.get(), offset);
        }

        // --- DECLARACION DE DATOS CON SUS DIRECTIVAS ---
        else if (auto data = dynamic_cast<const vm::DataDecl *>(node)) {
            // Validar que el label no esté vacío
            if (data->label.empty()) {
                throw std::runtime_error("Error: declaración de datos sin label.");
            }

            // Validar que el label no esté duplicado
            if (symbol_table.find(data->label) != symbol_table.end()) {
                throw std::runtime_error(
                    "Error: label duplicado: '" + data->label + "'"
                );
            }

            symbol_table[data->label] = offset;

            size_t elem_size = size_of_directive(data->directive);

            for (auto &expr: data->values) {
                if (auto s = dynamic_cast<vm::StringExpr *>(expr.get())) {
                    offset += s->value.size();
                } else {
                    offset += elem_size;
                }
            }
        }

        // --- INSTRUCCIONES Y DIRECTIVAS ---
        else if (auto instr = dynamic_cast<const vm::Instruction *>(node)) {
            {
                // Es una instrucción real?
                auto it = InstrTable.find(instr->opcode);

                if (it == InstrTable.end()) {
                    // No está en la tabla -> puede ser una pseudo-instrucción (directiva)
                    // o un error del usuario.

                    if (PseudoInstructions.count(instr->opcode)) {
                        // No ocupan espacio, configuran el entorno / emisor
                        apply_directive(instr);
                        return;
                    }

                    throw std::runtime_error(
                        "Error: instruccion o directiva desconocida: '" + instr->opcode + "'"
                    );
                }

                // Instrucción válida -> sumar su tamaño
                offset += size_of_instruction(instr);
            }
        }
    }


    void Assembler::apply_directive(const vm::Instruction *instr) {
        const std::string &op = instr->opcode;

        // --- org ---
        if (op == "org") {
            if (instr->operands.size() != 1)
                throw std::runtime_error("Error: org requiere 1 operando.");

            if (true) {
                uint64_t new_addr = eval_operand(instr->operands[0].get());
                current_offset    = default_address = new_addr;
            } else {
                instr->operands[0]->print(4);
                throw std::runtime_error("Error: org requiere 2 operando.");
            }

            return;
        }

        // --- align ---
        if (op == "align") {
            if (instr->operands.size() != 1)
                throw std::runtime_error("Error: align requiere 1 operando.");

            vm::NumberOperand *number = dynamic_cast<vm::NumberOperand *>(instr->operands[0].get());
            uint64_t align = eval_operand(number);

            if (align == 0 || (align & (align - 1)) != 0)
                throw std::runtime_error("Error: align debe ser potencia de 2.");

            while (current_offset % align != 0) {
                output.push_back(0x00), current_offset++;
            }

            return;
        }

        throw std::runtime_error("Error: directiva desconocida: " + op);
    }
}
