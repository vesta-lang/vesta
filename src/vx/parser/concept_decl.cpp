/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file concept_decl.cpp
 * @brief `concept N<Self, ...> = pred;` / `{ sentencias }` / `{ miembros }`.
 *
 * La forma de MIEMBROS se escribe como el cuerpo de un struct -- campos,
 * metodos exigidos (`;`) y metodos por defecto (con cuerpo) -- y la analiza
 * el mismo codigo que el cuerpo de un struct: un concepto que describe un
 * tipo no tiene por que aprender otra gramatica para hacerlo.
 */

#include "vx/parser.h"

namespace vx {
namespace {

/**
 * @enum MemberTail
 * @brief Que queda de un miembro tras su `<tipo> <nombre>`.
 */
enum class MemberTail : uint8_t {
    Method, ///< firma y luego `;`, `=> expr;` o `{ cuerpo }`
    Field,  ///< `= valor;`, `[N];`, `: bits;` -- sus llaves son un valor
};

/**
 * @brief Salta hasta el final de un miembro sin consumir nada.
 *
 * Cuenta parentesis, corchetes y llaves.  En un metodo, unas llaves al nivel
 * del miembro son su CUERPO y lo terminan (salvo tras `=>`, donde son un
 * valor); en un campo siempre son un valor.
 *
 * @param ml   El lexer (lookahead).
 * @param k    Posicion de lookahead donde empieza lo que queda.
 * @param tail Que clase de miembro es.
 * @return La posicion tras el miembro, o 0 si el cuerpo se acaba antes.
 */
size_t skip_member_tail(Lexer &ml, size_t k, MemberTail tail) {
    int depth = 0;
    bool after_arrow = false;
    for (;; ++k) {
        const TokenKind tk = ml.peek_at(k).kind;
        if (tk == TokenKind::END_OF_FILE) return 0;
        if (depth == 0) {
            if (tk == TokenKind::SEMICOLON) return k + 1;
            if (tk == TokenKind::RBRACE) return 0; // se acabo el concepto
            if (tk == TokenKind::FAT_ARROW) after_arrow = true;
            if (tk == TokenKind::LBRACE && tail == MemberTail::Method &&
                !after_arrow) {
                /* El cuerpo: acaba en su llave de cierre. */
                int braces = 0;
                for (;; ++k) {
                    const TokenKind bk = ml.peek_at(k).kind;
                    if (bk == TokenKind::END_OF_FILE) return 0;
                    if (bk == TokenKind::LBRACE) ++braces;
                    if (bk == TokenKind::RBRACE && --braces == 0) return k + 1;
                }
            }
        }
        if (tk == TokenKind::LPAREN || tk == TokenKind::LBRACKET ||
            tk == TokenKind::LBRACE)
            ++depth;
        else if (tk == TokenKind::RPAREN || tk == TokenKind::RBRACKET ||
                 tk == TokenKind::RBRACE)
            --depth;
    }
}

} // namespace

size_t Parser::peek_skip_concept_member_(size_t at) const {
    Lexer &ml = const_cast<Lexer &>(lex_);
    switch (ml.peek_at(at).kind) {
    case TokenKind::SEMICOLON: return at + 1; // campo
    case TokenKind::LPAREN:
    case TokenKind::LT: return skip_member_tail(ml, at, MemberTail::Method);
    case TokenKind::ASSIGN:
    case TokenKind::LBRACKET:
    case TokenKind::COLON: return skip_member_tail(ml, at, MemberTail::Field);
    default: return 0;
    }
}

bool Parser::concept_body_declares_members_() const {
    Lexer &ml = const_cast<Lexer &>(lex_);
    size_t k = 0; // el primer token tras la `{` (que es current_)
    size_t members = 0;
    for (;;) {
        const TokenKind tk = ml.peek_at(k).kind;
        if (tk == TokenKind::RBRACE) return members > 0;
        if (tk == TokenKind::END_OF_FILE) return false;
        /* Anotaciones y modificadores delante del miembro. */
        if (tk == TokenKind::AT &&
            ml.peek_at(k + 1).kind == TokenKind::IDENTIFIER) {
            k += 2;
            if (ml.peek_at(k).kind == TokenKind::LPAREN) {
                int depth = 0;
                do {
                    const TokenKind pk = ml.peek_at(k).kind;
                    if (pk == TokenKind::END_OF_FILE) return false;
                    if (pk == TokenKind::LPAREN) ++depth;
                    if (pk == TokenKind::RPAREN) --depth;
                    ++k;
                } while (depth > 0);
            }
            continue;
        }
        if (tk == TokenKind::KW_PUBLIC || tk == TokenKind::KW_PRIVATE ||
            tk == TokenKind::KW_STATIC ||
            (tk == TokenKind::IDENTIFIER &&
             ml.peek_at(k).lexeme == "comptime")) {
            ++k;
            continue;
        }
        const size_t after_type = peek_skip_type(k);
        if (after_type == 0 || !is_name_token(ml.peek_at(after_type).kind))
            return false; // una sentencia: es un bloque
        const size_t end = peek_skip_concept_member_(after_type + 1);
        if (end == 0) return false;
        k = end;
        ++members;
    }
}

std::unique_ptr<ast::ConceptDecl> Parser::parse_concept_decl() {
    auto c = std::make_unique<ast::ConceptDecl>();
    c->loc = current_.loc;
    (void)consume(); // 'concept' (identificador contextual)
    if (current_.kind != TokenKind::IDENTIFIER) {
        error_here("se esperaba el nombre del concepto tras 'concept'");
        return nullptr;
    }
    c->name = consume().lexeme;
    // Type-params opcionales `<T>` / `<K, V>`.
    if (current_.kind == TokenKind::LT) {
        (void)consume(); // '<'
        while (current_.kind == TokenKind::IDENTIFIER) {
            c->type_params.push_back(consume().lexeme);
            if (!match(TokenKind::COMMA)) break;
        }
        (void)expect_close_angle(
            "se esperaba '>' al cerrar los parametros del concepto");
    }

    if (current_.kind == TokenKind::ASSIGN) {
        // Forma predicado: `concept N<T> = <bool-expr>;`.
        (void)consume(); // '='
        c->ckind = ast::ConceptKind::Predicate;
        // Registrar T como alias temporal para `(T)x` / `is_x<T>()`.
        const auto temp = register_temp_type_aliases(c->type_params);
        c->predicate = parse_expr();
        unregister_temp_type_aliases(temp);
        (void)expect(TokenKind::SEMICOLON,
                     "se esperaba ';' tras el predicado del concepto");
        return c;
    }

    if (current_.kind != TokenKind::LBRACE) {
        error_here("se esperaba '=' o '{' tras el nombre del concepto");
        return c;
    }

    const bool members = concept_body_declares_members_();
    (void)consume(); // '{'

    if (members) {
        /* Los miembros los lee el cuerpo de struct.  Sin nombre: un concepto
         * no tiene constructor ni destructor que reconocer por el. */
        c->ckind = ast::ConceptKind::Structural;
        ast::StructDecl shape;
        shape.loc = c->loc;
        shape.type_params = c->type_params;
        parse_struct_body_(shape, MemberBodyOwner::Concept);
        c->fields = std::move(shape.fields);
        c->methods = std::move(shape.methods);
        (void)expect(TokenKind::RBRACE,
                     "se esperaba '}' al cerrar el concepto estructural");
        return c;
    }

    // Forma bloque: `concept N<T> { <stmts comptime>; return <bool>; }`.
    c->ckind = ast::ConceptKind::Block;
    auto blk = std::make_unique<ast::BlockStmt>();
    blk->loc = c->loc;
    const auto temp = register_temp_type_aliases(c->type_params);
    while (current_.kind != TokenKind::RBRACE &&
           current_.kind != TokenKind::END_OF_FILE) {
        if (!parse_statement_into(blk->body)) break;
    }
    unregister_temp_type_aliases(temp);
    (void)expect(TokenKind::RBRACE,
                 "se esperaba '}' al cerrar el cuerpo del concepto");
    c->body = std::move(blk);
    return c;
}

} // namespace vx
