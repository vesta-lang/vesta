/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file type_lookahead.cpp
 * @brief Mirar hacia delante si empieza un TIPO, y donde acaba, sin consumir.
 *
 * Decidir que forma tiene algo -- una firma de metodo, una declaracion -- pide
 * saltar el tipo que va delante.  Mirando solo un token, un tipo generico o
 * cualificado (`Optional<E>`, `mat.Vec`) no se reconocia y la decision salia
 * al reves.
 */

#include "vx/parser.h"

namespace vx {

/// Hasta donde se mira antes de darlo por no-tipo: un tipo real es corto.
static constexpr size_t kMaxTypeLookahead = 64;

size_t Parser::peek_skip_type(size_t at) const {
    Lexer &ml = const_cast<Lexer &>(lex_);
    const Token &first = ml.peek_at(at);
    const bool starts =
        primitive_kind_from_token(first.kind) != PrimitiveKind::COUNT ||
        first.kind == TokenKind::KW_VOID || first.kind == TokenKind::IDENTIFIER;
    if (!starts) return 0;
    size_t k = at + 1;
    /* Cualificado: `a.b.Tipo`. */
    while (ml.peek_at(k).kind == TokenKind::DOT &&
           ml.peek_at(k + 1).kind == TokenKind::IDENTIFIER)
        k += 2;
    /* Argumentos de tipo, anidados: `<...>`, que pueden cerrar dos a la vez
     * con `>>`. */
    if (ml.peek_at(k).kind == TokenKind::LT) {
        int depth = 0;
        for (; k < at + kMaxTypeLookahead; ++k) {
            const TokenKind tk = ml.peek_at(k).kind;
            if (tk == TokenKind::LT) {
                ++depth;
            } else if (tk == TokenKind::GT) {
                if (--depth == 0) {
                    ++k;
                    break;
                }
            } else if (tk == TokenKind::SHR) {
                depth -= 2;
                if (depth <= 0) {
                    ++k;
                    break;
                }
            } else if (tk == TokenKind::SEMICOLON ||
                       tk == TokenKind::LBRACE ||
                       tk == TokenKind::END_OF_FILE) {
                return 0; // no cerro: no era un tipo
            }
        }
        if (depth > 0) return 0;
    }
    /* Sufijos: punteros, arrays y opcional. */
    for (;;) {
        const TokenKind tk = ml.peek_at(k).kind;
        if (tk == TokenKind::STAR || tk == TokenKind::QUESTION) {
            ++k;
        } else if (tk == TokenKind::LBRACKET) {
            while (ml.peek_at(k).kind != TokenKind::RBRACKET &&
                   ml.peek_at(k).kind != TokenKind::END_OF_FILE &&
                   k < at + kMaxTypeLookahead)
                ++k;
            if (ml.peek_at(k).kind != TokenKind::RBRACKET) return 0;
            ++k;
        } else {
            break;
        }
    }
    return k;
}

bool Parser::is_known_type_head(size_t at) const {
    Lexer &ml = const_cast<Lexer &>(lex_);
    const Token &t = ml.peek_at(at);
    if (primitive_kind_from_token(t.kind) != PrimitiveKind::COUNT ||
        t.kind == TokenKind::KW_VOID)
        return true;
    if (t.kind != TokenKind::IDENTIFIER) return false;
    // `ns.Tipo` de un namespace importado.
    if (imported_namespaces_.count(t.lexeme) > 0)
        return ml.peek_at(at + 1).kind == TokenKind::DOT &&
               ml.peek_at(at + 2).kind == TokenKind::IDENTIFIER;
    return declared_structs_.count(t.lexeme) > 0 ||
           declared_aliases_.count(t.lexeme) > 0 ||
           declared_nominal_types_.count(util::InternedName::intern(t.lexeme)) >
               0 ||
           is_active_type_param(t.lexeme);
}

bool Parser::looks_like_compound_literal() const {
    // Precondicion: current_ es '('.  Patron: `( TIPO ) {`, con TIPO
    // empezando por un tipo conocido.
    if (!is_known_type_head(0)) return false;
    Lexer &ml = const_cast<Lexer &>(lex_);
    // Tras un TIPO, un `.` es un miembro, no un tipo cualificado:
    // `if (Config.debug) {` y `match (Color.Rojo) {` no son literales.  Solo
    // un namespace importado se cualifica.
    if (ml.peek_at(1).kind == TokenKind::DOT &&
        imported_namespaces_.count(ml.peek_at(0).lexeme) == 0)
        return false;
    const size_t after = peek_skip_type(0);
    if (after == 0) return false;
    return ml.peek_at(after).kind == TokenKind::RPAREN &&
           ml.peek_at(after + 1).kind == TokenKind::LBRACE;
}

} // namespace vx
