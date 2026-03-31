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
            return it->second->address;
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
            return symbol_table[l->name]->address;

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
        : default_address(vm::mem::CODE_BEGIN) // dirección base
    {
        symbol_table.clear();
    }

    void Assembler::compute_label_sizes() {
        for (auto &[spaceName, space]: ctx.space_address) {
            for (auto &[sectionName, section]: space.table_section) {
                // Obtener labels ordenados por offset
                std::vector<Label *> ordered;
                ordered.reserve(section.table_label.size());

                for (auto &[name, lbl]: section.table_label)
                    ordered.push_back(&lbl);

                std::sort(ordered.begin(), ordered.end(),
                          [](Label *a, Label *b) {
                              return a->address < b->address;
                          });

                // Calcular tamaños
                for (size_t i = 0; i < ordered.size(); ++i) {
                    uint64_t start = ordered[i]->address;
                    uint64_t end;

                    if (i + 1 < ordered.size())
                        end = ordered[i + 1]->address;
                    else
                        end = section.size_real; // último label

                    ordered[i]->size = end - start;
                }
            }
        }
    }

    std::vector<uint8_t> Assembler::assemble(
        const std::vector<std::unique_ptr<vm::ASTNode> > &ast) {
        uint64_t offset = 0;

        // por ahora esta seccion y espacio contendra la meta informacion y la añade
        // el ensamblador
        ctx.add_space("MetaSpace", 0x0, 0x0);
        ctx.get_space("MetaSpace")->add_section("strings", 0x0, 0x0);

        // 1 Primera pasada
        for (auto &node: ast)
            first_pass(node.get(), offset);

        current_section = nullptr;
        current_label = nullptr;

        // conociendo el offset de cada seccion y label, se puede
        // calcular el tamaño de cada label
        //compute_label_sizes();

        for (auto &node: ast)
            emit_pass(node.get());


        // 2 Segunda pasada (datos)
        /*for (auto &node: ast)
            second_pass_data(node.get());

        current_offset = 0;

        // 3 Tercera pasada (código)
        for (auto &node: ast)
            third_pass_code(node.get());*/

        // una vez realizado todas las fases de analisis de etiquetas
        // y emision de codigo, se puede computar las direcciones de inicio
        // y final de cada seccion.
        ctx.compute_all_ranges();
        return output.output;
    }


    void Assembler::emit_pass(const vm::ASTNode *node) {
        // Si el nodo es una etiqueta (LabelNode)
        if (auto lab = dynamic_cast<const vm::LabelNode *>(node)) {
            if (current_section == nullptr) throw std::runtime_error("Label no definido: " + lab->name);
            current_label = current_section->get_label(lab->name);
            // Recorre todos los nodos dentro del cuerpo de la etiqueta
            // y vuelve a llamar a emit_pass recursivamente.
            for (auto &child: lab->body)
                emit_pass(child.get());
        }

        // Si el nodo es una declaración de datos
        else if (auto data = dynamic_cast<const vm::DataDecl *>(node)) {
            emit_data(data); // Llama al manejador específico para datos
        }

        // debemos evaluar las notaciones section, para poder averiguar en que seccion
        // y label se encuentra el codigo.
        else if (auto annotation = dynamic_cast<const vm::AnnotationNode *>(node)) {
            if (annotation->key == "Section") {
                std::string section_name;
                for (const auto &child
                     : annotation->children) {
                    if (child->key == "Name") {
                        section_name = child->value;
                        break;
                    }
                }

                this->current_section = this->ctx.get_section(section_name);
            }
        }
        // Si el nodo es una instrucción
        else if (auto instr = dynamic_cast<const vm::Instruction *>(node)) {
            // Si la instrucción es una pseudo-instrucción (directiva)
            if (PseudoInstructions.count(instr->opcode)) {
                apply_directive(instr); // Aplica la directiva correspondiente
            } else {
                // Si es una instrucción real, emite su código máquina,
                // se debe conocer la seccion y label
                emit_instruction(instr);
            }
        }
    }


    void Assembler::first_pass(const vm::ASTNode *node, uint64_t &offset) {
        // --- LABELS ---
        if (auto lab = dynamic_cast<const vm::LabelNode *>(node)) {
            // solo aplicar si el formato es velb
            if (!current_section && ctx.format_output == "velb")
                throw std::runtime_error("Label fuera de una seccion");

            // offset inicial de la label
            uint64_t start = offset;

            // registrar la label en la sección, tamaño temporal = 0
            if (current_section != nullptr) {
                current_section->add_label(lab->name, offset, 0);
                current_label = current_section->get_label(lab->name);
            }

            symbol_table[lab->name] = current_label;

            // Guardar sección actual, al usar first_pass puede cambiar
            // si hay un label vacio con una notacion section despues
            Section *saved_section = current_section;

            // procesar el cuerpo de la label
            for (auto &child: lab->body)
                first_pass(child.get(), offset);

            // ahora offset ha avanzado -> calcular tamaño real
            uint64_t size = offset - start;

            // actualizar tamaño real en la sección
            if (saved_section)
                saved_section->update_label_size(lab->name, size);
        }

        // para nodos de tipo anotacion, no todos los nodos de este tipo, se tienen en cuenta.
        else if (auto data = dynamic_cast<const vm::AnnotationNode *>(node)) {
            apply_annotation(data);
        }

        // --- DECLARACION DE DATOS CON SUS DIRECTIVAS ---
        else if (auto data = dynamic_cast<const vm::DataDecl *>(node)) {
            if (!current_section && ctx.format_output == "velb")
                throw std::runtime_error("Label fuera de una seccion");

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

            size_t elem_size = size_of_directive(data->directive);
            size_t offset_label = offset; // debemos guardar el offset de la label,
            // ya que se modificara en el for el offset global y se perdera la referencia

            // guarda el tamaño real de la label
            size_t size_of_label = 0;
            for (auto &expr: data->values) {
                if (auto s = dynamic_cast<vm::StringExpr *>(expr.get())) {
                    size_of_label += s->value.size();
                    offset += s->value.size();
                } else {
                    size_of_label += elem_size;
                    offset += elem_size;
                }

                // guardamos el tamaño real de la seccion
                if (current_section) {
                    current_section->size_real = offset;
                }
            }


            // añadir a la sección actual el nuevo label
            current_section->add_label(data->label, offset_label, size_of_label);

            // añadimos la label a la seccion, una vez obtenida su tamaño real
            symbol_table[data->label] = current_section->get_label(data->label);
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
                        if (instr->opcode == "align") {
                            /* si es align, en la primera fase se debe aplicar aqui, ya que
                                                       * sino offset y current_offset se desincronizaran.
                                                       */
                            // No ocupan espacio, configuran el entorno / emisor
                            vm::NumberOperand *number = dynamic_cast<vm::NumberOperand *>(instr->operands[0].get());
                            uint64_t align = eval_operand(number);

                            if (align == 0 || (align & (align - 1)) != 0)
                                throw std::runtime_error("Error: align debe ser potencia de 2.");

                            // alineamos el offset local al tamaño indicado.
                            offset = (offset + align - 1) & ~(align - 1);
                        } else apply_directive(instr);
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

    void Assembler::apply_annotation(const vm::AnnotationNode *annotation) {
        // si la notacion esta permitida por el ensamblador entonces tiene efecto y describe alguna informacion
        // necesaria para este ensamblador.
        auto it = annotation_handlers.find(annotation->key);
        if (it != annotation_handlers.end()) {
            it->second(annotation, *this); // Ejecuta la función asociada
            return;
        }

        // si no era una notacion permitida por el ensamblador, se mira si es una notacion de tipo documentacion,
        // estas notaciones no tienen ningun efecto en el ensamblador en primer lugar
        if (annotation_allow_doc.find(annotation->key) != annotation_allow_doc.end()) {
            // mas adeltante, generar secciones de documentacion + metadatos con esta informacion.
            return;
        }

        throw std::runtime_error("Error: la notacion: " + annotation->key + " no es una notacion valida");
    }

    void Assembler::apply_directive(const vm::Instruction *instr) {
        const std::string &op = instr->opcode;

        // --- org ---
        /*if (op == "org") {
            if (instr->operands.size() != 1)
                throw std::runtime_error("Error: org requiere 1 operando.");

            if (true) {
                uint64_t new_addr = eval_operand(instr->operands[0].get());
                current_offset = default_address = new_addr;
            } else {
                instr->operands[0]->print(4);
                throw std::runtime_error("Error: org requiere 2 operando.");
            }

            return;
        }*/

        // --- align ---
        if (op == "align") {
            if (instr->operands.size() != 1)
                throw std::runtime_error("Error: align requiere 1 operando.");

            vm::NumberOperand *number = dynamic_cast<vm::NumberOperand *>(instr->operands[0].get());
            uint64_t align = eval_operand(number);

            if (align == 0 || (align & (align - 1)) != 0)
                throw std::runtime_error("Error: align debe ser potencia de 2.");

            while (output.offset % align != 0) {
                output.emit8(0x00);
            }

            return;
        }

        if (op == "import") {
            if (instr->operands.size() != 1)
                throw std::runtime_error("Error: import requiere un string don el archivo a incluir.");
        }

        throw std::runtime_error("Error: directiva desconocida: " + op);
    }
}
