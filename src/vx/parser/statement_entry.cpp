/**
 * @file statement_entry.cpp
 * @brief Por donde se entra a leer una sentencia: donde va UNA o en una LISTA.
 *
 * Una declaracion de varios nombres son varias sentencias.  En una lista (un
 * bloque, un `comptime { }`) entran seguidas; donde el lenguaje pone una sola
 * -- el cuerpo de un `if` sin llaves -- se envuelven en un bloque, que es el
 * alcance que ya tenian.  Los dos caminos pasan por el mismo lector, que es
 * ademas el que mide lo que ocupa cada sentencia.
 *
 * @see vx/parser.h
 */
#include "vx/parser.h"

#include <utility>

namespace vx {

/**
 * El nodo se queda con la posicion de su primer token, cuya longitud es la de
 * ESE TOKEN y no la de la sentencia: una que empiece por `return` media seis
 * caracteres, los de la palabra clave, y al subrayar un fallo se marcaba la
 * palabra clave en vez de lo que se estaba evaluando.  Aqui se mide de verdad,
 * del primer byte de la sentencia al ultimo consumido, en el envoltorio y no
 * en cada rama: son decenas y bastaria olvidar una.
 */
std::unique_ptr<ast::Stmt> Parser::parse_statement_measured(
    std::vector<std::unique_ptr<ast::Stmt>> &rest) {
    const uint32_t ini = current_.loc.offset;
    auto st = parse_statement_inner();
    take_pending_declarators_(st.get(), rest);
    if (st && st->loc.offset >= ini) {
        // El final es donde empieza el token que YA no es de la sentencia.
        const uint32_t fin = current_.loc.offset;
        if (fin > st->loc.offset) st->loc.length = fin - st->loc.offset;
    }
    return st;
}

std::unique_ptr<ast::Stmt> Parser::parse_statement() {
    std::vector<std::unique_ptr<ast::Stmt>> rest;
    auto st = parse_statement_measured(rest);
    if (!st || rest.empty()) return st;
    auto blk = std::make_unique<ast::BlockStmt>();
    blk->loc = st->loc;
    blk->body.push_back(std::move(st));
    for (auto &s : rest)
        blk->body.push_back(std::move(s));
    return blk;
}

bool Parser::starts_control_statement_() {
    switch (current_.kind) {
    case TokenKind::KW_FOR:
    case TokenKind::KW_WHILE:
    case TokenKind::KW_DO:
    case TokenKind::KW_IF: return true;
    default: break;
    }
    // `foreach` es contextual: solo abre un bucle seguido de `(`.
    return current_.kind == TokenKind::IDENTIFIER &&
           current_.lexeme == "foreach" &&
           lex_.peek_at(0).kind == TokenKind::LPAREN;
}

bool Parser::parse_statement_into(
    std::vector<std::unique_ptr<ast::Stmt>> &out) {
    std::vector<std::unique_ptr<ast::Stmt>> rest;
    auto st = parse_statement_measured(rest);
    if (!st) return false;
    out.push_back(std::move(st));
    for (auto &s : rest)
        out.push_back(std::move(s));
    return true;
}

} // namespace vx
