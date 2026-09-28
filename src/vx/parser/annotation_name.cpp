/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file annotation_name.cpp
 * @brief El nombre de una anotacion tras el `@`, con sus partes.
 *
 * Una anotacion se nombra como cualquier otra cosa del lenguaje: un nombre, o
 * varios unidos por punto, igual que `type.size`.  La familia `@No.X`
 * (`@No.Inject`) es la primera que lo usa.  El nombre ENTERO es lo que se
 * busca en la tabla (@c annotation_names.h): `No` suelto no es nada, y una
 * errata en la segunda parte sigue siendo una errata.
 */

#include "vx/parser.h"

namespace vx {

std::string Parser::read_annotation_name_() {
    std::string name = consume().lexeme; // el IDENT tras el '@'
    while (current_.kind == TokenKind::DOT &&
           lex_.peek_at(0).kind == TokenKind::IDENTIFIER) {
        (void)consume(); // '.'
        name += '.';
        name += consume().lexeme;
    }
    return name;
}

size_t Parser::peek_skip_annotation_name_(size_t at) const {
    Lexer &ml = const_cast<Lexer &>(lex_);
    size_t k = at + 1; // tras el IDENT
    while (ml.peek_at(k).kind == TokenKind::DOT &&
           ml.peek_at(k + 1).kind == TokenKind::IDENTIFIER)
        k += 2;
    return k;
}

} // namespace vx
