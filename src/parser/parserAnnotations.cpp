/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file parserAnnotations.cpp
 * @brief Implementacion del parseo de anotaciones del parser de VestaVM.
 *
 * Contiene los metodos de @c vm::Parser responsables de reconocer y construir
 * nodos @c AnnotationNode del AST (directivas @SpaceAddress, @Section, @Format,
 * @InitPc, @Import, etc.).
 */
#include "parser/parser.h"

#include <iomanip>

namespace vm {
std::unique_ptr<AnnotationNode> Parser::parse_annotation() {
    advance(); // consumir '@'

    Token tok = expectToken(TokenType::IDENTIFIER, "Expected IDENTIFIER");
    if (tok.type != TokenType::IDENTIFIER) exit(-1);
    advance(); // consumir identificador

    auto node = std::make_unique<AnnotationNode>(tok.lexeme);

    if (current.type == TokenType::LBRACE) {
        advance(); // consumir '{'

        while (current.type != TokenType::RBRACE &&
               peek().type != TokenType::EndOfFile) {
            if (current.type == TokenType::AT) {
                node->add_child(parse_annotation()); // recursion
            } else {
                advance(); // ignorar tokens irrelevantes
            }
        }

        if (current.type == TokenType::RBRACE) advance(); // consumir '}'
    } else if (current.type == TokenType::LPAREN) {
        advance();                    // consumir )
        node->value = current.lexeme; // obtener el valor de la notacion
        advance();                    // consumir valor
        expect(TokenType::RPAREN, "Se esperaba un )");
    } else {
        error(current,
              "se esperaba un { o un ( y se encontro un: " + tok.lexeme);
    }

    if (tok.lexeme == "@Class") {
    }

    return node;
}
} // namespace vm
