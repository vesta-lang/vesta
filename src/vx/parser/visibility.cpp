/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file visibility.cpp
 * @brief La visibilidad escrita delante de un miembro.
 *
 * Los cuerpos de struct, clase e `impl` la leian cada uno a su manera: la
 * clase aceptaba `protected`, el struct no, el `impl` solo `private`, y el
 * struct la tiraba despues de leerla.  Un solo sistema de visibilidad pide un
 * solo lector.
 */

#include "vx/parser.h"

namespace vx {

bool Parser::parse_member_visibility_(ast::Visibility &out, bool &seen) {
    ast::Visibility v = ast::Visibility::Unwritten;
    switch (current_.kind) {
    case TokenKind::KW_PUBLIC: v = ast::Visibility::Public; break;
    case TokenKind::KW_PRIVATE: v = ast::Visibility::Private; break;
    case TokenKind::KW_PROTECTED: v = ast::Visibility::Protected; break;
    case TokenKind::IDENTIFIER: {
        /* `internal` es contextual: no reserva el nombre.  Es visibilidad
         * solo si detras sigue un miembro, no si es el nombre de algo. */
        if (current_.lexeme != "internal") return false;
        const TokenKind next = lex_.peek_at(0).kind;
        if (next == TokenKind::LPAREN || next == TokenKind::SEMICOLON ||
            next == TokenKind::ASSIGN || next == TokenKind::COMMA ||
            next == TokenKind::DOT)
            return false;
        v = ast::Visibility::Internal;
        break;
    }
    default: return false;
    }
    if (seen)
        diags_.diag(current_.loc, DiagLevel::ERR, "VXP098",
                    {current_.lexeme});
    seen = true;
    out = v;
    (void)consume();
    return true;
}

} // namespace vx
