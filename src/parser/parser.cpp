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

#include "parser/parser.h"

#include <iomanip>

#include "Levenshtein.hpp"

namespace vm {
    static const std::unordered_map<std::string, InstructionPattern> InstructionSet = {
        // TWO operandos
        {"mov", {"mov", OpArity::TWO}},

        {"addu", {"addu", OpArity::TWO}},
        {"subu", {"subu", OpArity::TWO}},
        {"mulu", {"mulu", OpArity::TWO}},
        {"divu", {"divu", OpArity::TWO}},
        {"cmpu", {"cmpu", OpArity::TWO}},

        {"adds", {"adds", OpArity::TWO}},
        {"subs", {"subs", OpArity::TWO}},
        {"muls", {"muls", OpArity::TWO}},
        {"divs", {"divs", OpArity::TWO}},
        {"cmps", {"cmps", OpArity::TWO}},

        // ZERO operandos
        {"throw", {"nop1", OpArity::ZERO}},
        {"nop1", {"nop1", OpArity::ZERO}},
        {"nop2", {"nop2", OpArity::ZERO}},
        {"hlt", {"hlt", OpArity::ZERO}},
        {"ret", {"ret", OpArity::ZERO}},
        {"resbp", {"resbp", OpArity::ZERO}},
        {"leave", {"leave", OpArity::ZERO}},
        {"enter", {"enter", OpArity::ZERO}},

        // ----------------------------------------------------------------------
        // ONE operando

        // aunque no es una instruccion, se detectara como una
        {"org", {"org", OpArity::ONE}},

        {"jmp", {"jmp", OpArity::ONE}},
        {"call", {"call", OpArity::ONE}},
        {"callvm", {"callvm", OpArity::ONE}},
        {"push", {"push", OpArity::ONE}},
        {"pop", {"pop", OpArity::ONE}},
        {"inc", {"inc", OpArity::ONE}},
        {"dec", {"dec", OpArity::ONE}},
        {"vminfo", {"vminfo", OpArity::ONE}},
        {"vminfomanager", {"vminfomanager", OpArity::ONE}},


    };

    bool Parser::match(TokenType type) {
        if (current.type == type) {
            advance();
            return true;
        }
        return false;
    }

    bool Parser::expect(TokenType type, const std::string &msg) {
        if (current.type == type) {
            advance();
            return true;
        }
        error(current, msg);
        return false;
    }




    void Parser::error(const Token &tok, const std::string &msg) {
        throw ParseError(tok.line, tok.column,
                         "Parser ERROR: " + msg +
                         " [" + tok.lexeme + "]");
    }

    void Parser::warning(int line, int col, const std::string &msg, const std::string &sugg = "") {
        warnings.emplace_back(line, col, msg, sugg);
        std::cout << "Linea " << line << ":" << col
                << " - " << msg << (sugg.empty() ? "" : " (" + sugg + "?)") << std::endl;
    }


    void Parser::print_warnings() const {
        if (!warnings.empty()) {
            std::cout << "\n" << warnings.size() << " warnings encontrados:\n";
            for (const auto &w: warnings) {
                std::cout << "    Línea " << w.line << ":" << w.column
                        << " - " << w.what()
                        << (w.suggestion.empty() ? "" : " (sugerencia: " + w.suggestion + ")")
                        << std::endl;
            }
        }
    }

    std::vector<std::unique_ptr<ASTNode> > Parser::parse() {
        std::vector<std::unique_ptr<ASTNode> > program;

        while (current.type != TokenType::EndOfFile) {
            std::unique_ptr<ASTNode> node = nullptr;
            Token                    next = peek();
            // IDENTIFIER + COLON? -> SECCIÓN
            if (current.type == TokenType::IDENTIFIER && peek().type == TokenType::COLON) {
                node = parse_section();
            }
            // IDENTIFIER solo? -> INSTRUCCIÓN
            // IDENTIFIER IDENTIFIER? -> posible instruccion de dos identificadores
            else if (
                (current.type == TokenType::IDENTIFIER) ||
                (current.type == TokenType::IDENTIFIER) && peek().type == TokenType::IDENTIFIER
            ) {
                node = parse_statement();
            }

            // se encontro una o varias anotaciones
            else if (current.type == TokenType::AT) {
                node = parse_annotation();
            }

            // Skip tokens inválidos
            else {
                std::cerr << "linea: " << current.line << ":" << current.column <<
                        " Parser ERROR: " << "Skipping invalid token: " + token_type_to_string(current.type) +
                        " [" + current.lexeme + "]" << std::endl;
                advance();
                continue;
            }

            if (node) program.push_back(std::move(node));
        }

        return program;
    }


    std::unique_ptr<ASTNode> Parser::parse_section() {
        if (current.type != TokenType::IDENTIFIER) {
            return nullptr;
        }

        // guardar nombre de la seccion:
        std::string section_name = current.lexeme;
        advance(); // consumimos el token

        // esperamos q haya dos puntos despues del ID de una section.
        expect(TokenType::COLON, "Expected COLON");

        std::vector<std::unique_ptr<ASTNode> > body = parse();

        return std::make_unique<SectionNode>(section_name, std::move(body));
    }

    std::unique_ptr<ASTNode> Parser::parse_operand() {
        switch (current.type) {
            case TokenType::REGISTER: {
                std::string reg = current.lexeme;
                advance();

                // Decodificar tamaño (r0b=8bit, r0w=16bit, etc.)
                int size_bits = 64; // Default 64-bit
                if (reg.size() > 2) {
                    char suffix = reg.back();

                    // Solo quitar el sufijo si realmente es un sufijo válido
                    if (suffix == 'b' || suffix == 'w' || suffix == 'd') {
                        reg.pop_back();
                        switch (suffix) {
                            case 'b': size_bits = 8; break;
                            case 'w': size_bits = 16; break;
                            case 'd': size_bits = 32; break;
                        }
                    }
                }

                return std::make_unique<RegisterOperand>(reg, size_bits);
            }

            case TokenType::NUMBER_DEC:
            case TokenType::NUMBER_HEX:
            case TokenType::NUMBER_BIN:
            case TokenType::NUMBER_OCT: {
                auto num = std::make_unique<NumberOperand>(
                    current.lexeme, current.type);
                advance();
                return std::move(num);
            }

            case TokenType::STRING: {
                auto str = std::make_unique<StringOperand>(current.lexeme);
                advance();
                return std::move(str);
            }

            case TokenType::IDENTIFIER: {
                // Label o variable
                auto label = std::make_unique<LabelOperand>(current.lexeme);
                advance();
                return std::move(label);
            }

            default:
                error(current, "Operando inválido");
                advance(); // Skip
                return nullptr;
        }
    }

    std::unique_ptr<ASTNode> Parser::parse_data_directive() {
        // PATRONES:
        // msg db "Hola mundo"
        // bytes db 0xFF, 0x00, 0x11
        // count dw 42, 100

        std::string label = current.lexeme; // "msg", "bytes"
        advance();                          // Consumir label

        // Esperar directiva: db, dw, dd, ptr, etc
        Token directiveTok = expectToken(TokenType::DATA_DIRECTIVE, "Expected data directive (dq, db, dw, dd, ptr)");
        if (directiveTok.type != TokenType::DATA_DIRECTIVE) {
            return nullptr;
        }
        advance(); // consumir la directiva

        std::string                             directive = directiveTok.lexeme; // "db", "dw", "dd"
        std::vector<std::unique_ptr<ExprNode> > values;

        // Parsear valores: "Hola", 42, 0xFF, label, etc.
        while (current.type != TokenType::EndOfFile) {
            // STRING, NUMBER, IDENTIFIER (labels)
            switch (current.type) {
                case TokenType::STRING:
                    values.push_back(std::make_unique<StringExpr>(current.lexeme));
                    advance();
                    break;
                case TokenType::NUMBER_DEC:
                case TokenType::NUMBER_HEX:
                case TokenType::NUMBER_BIN:
                case TokenType::NUMBER_OCT:
                case TokenType::IDENTIFIER:
                case TokenType::MINUS: // si es un -, puede ser valor negativo?
                    values.push_back(parse_expression());
                    break;

                default:
                    error(current, "Invalid data value");
                    advance();
                    continue;
            }

            // Coma opcional
            if (match(TokenType::COMMA)) {
                continue; // Más datos
            }
            break; // Fin de datos
        }

        return std::make_unique<DataDecl>(std::move(label),
                                          std::move(directive),
                                          std::move(values));
    }


    std::unique_ptr<ASTNode> Parser::parse_instruction() {
        std::string opcode   = current.lexeme;
        auto        it       = InstructionSet.find(opcode);
        auto        valid_it = it;
        if (it == InstructionSet.end()) {
            float affinity = 0.0;
            int   dist     = 0;


            // intentamos recuperarnos del error ¿de sintaxis?
            for (auto &option: InstructionSet) {
                dist     = utils::Levenshtein::distance(opcode, option.first);
                affinity = utils::Levenshtein::affinity(opcode, option.first);

                // Top 3 resultados
                if (dist <= 3 || affinity >= 70) {
                    // si la distancia es <= 3 o la afinidad es mayor o igual al 70%
                    warning(current.line, current.column,
                            "Instruccion '" + current.lexeme + "' desconocida",
                            option.first);
                    opcode   = option.first;
                    valid_it = InstructionSet.find(option.first); // Actualizar
                    goto exit_error;                              // nos pudimos recuperar tal vez
                }
            }

            std::stringstream ss;
            for (auto &option: InstructionSet) {
                dist     = utils::Levenshtein::distance(opcode, option.first);
                affinity = utils::Levenshtein::affinity(opcode, option.first);

                if (affinity > 30) {
                    ss << "Instr: " << option.first << "\n"
                            << "Dist: " << dist << "\n"
                            << "Aff: " << std::fixed << std::setprecision(1)
                            << affinity * 100 << "%\n\n";
                }
            }

            error(current,
                  "Instruccion no existe esperado: " + opcode + "\n" + ss.str()
            );
            return nullptr;
        }
    exit_error:
        const auto &pattern = valid_it->second; // SIEMPRE válido

        advance(); // consumir el opcode

        std::vector<std::unique_ptr<ASTNode> > operands;

        switch (pattern.arity) {
            case OpArity::ZERO: break;
            case OpArity::ONE: operands.emplace_back(parse_operand());
                break;
            case OpArity::TWO:
                operands.emplace_back(parse_operand());
                expect(TokenType::COMMA, "Se esperaba ',' después de destino");
                operands.emplace_back(parse_operand());
                break;
        }

        // Usa pattern.opcode para codegen
        return std::make_unique<Instruction>(pattern.opcode, std::move(operands));
    }


    std::unique_ptr<ASTNode> Parser::parse_statement() {
        // Si es un registro y un identificador, se trata de una instruccion.

        Token next = peek();

        // directiva datos: msg db "Hola mundo"
        if (current.type == TokenType::IDENTIFIER && next.type == TokenType::DATA_DIRECTIVE) {
            return parse_data_directive();
        }

        if (
            (current.type == TokenType::IDENTIFIER && next.type == TokenType::REGISTER) ||
            (current.type == TokenType::IDENTIFIER && is_number_token(next.type)) ||
            (current.type == TokenType::IDENTIFIER && next.type == TokenType::IDENTIFIER)
        ) {
            /**
             * Si es identificador + registro ||
             * Si es identificador + numero
             */
            return parse_instruction();
        }

        // instrucciones de identificador unico sin operandos:
        if (current.lexeme == "nop1" || current.lexeme == "nop2" || current.lexeme == "ret") {
            return parse_instruction();
        }

        error(current, "Token no esperado: " + current.lexeme);
        advance(); // Skip
        return nullptr;
    }
}
