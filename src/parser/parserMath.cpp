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

namespace vm {
    std::unique_ptr<ExprNode> Parser::parse_expression() {
        auto left = parse_term();

        while (current.type == TokenType::PLUS ||
            current.type == TokenType::MINUS) {
            char op = current.lexeme[0];
            advance();
            auto right = parse_term();
            left       = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
        }

        return left;
    }

    std::unique_ptr<ExprNode> Parser::parse_term() {
        auto left = parse_factor();

        while (current.type == TokenType::STAR ||
            current.type == TokenType::SLASH) {
            char op = current.lexeme[0];
            advance();
            auto right = parse_factor();
            left       = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
        }

        return left;
    }

    std::unique_ptr<ExprNode> Parser::parse_factor() {
        if (current.type == TokenType::NUMBER_DEC ||
            current.type == TokenType::NUMBER_HEX ||
            current.type == TokenType::NUMBER_BIN ||
            current.type == TokenType::NUMBER_OCT) {
            auto node = std::make_unique<NumberExpr>(current.lexeme);
            advance();
            return node;
        }

        if (current.type == TokenType::IDENTIFIER) {
            auto node = std::make_unique<LabelExpr>(current.lexeme);
            advance();
            return node;
        }

        if (match(TokenType::LPAREN)) {
            auto expr = parse_expression();
            expect(TokenType::RPAREN, "Expected ')'");
            return expr;
        }

        error(current, "Invalid expression");
        return nullptr;
    }
}
