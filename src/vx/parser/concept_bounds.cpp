/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file concept_bounds.cpp
 * @brief La lista de conceptos de una cota: `C + ns.D<i64, T> + ...`.
 *
 * La leian tres sitios -- los parametros con cota, la cabeza generica y el
 * `where` --, cada uno con su copia, y ninguna aceptaba argumentos: `<V:
 * View<i64>>` no se podia escribir.  Aqui esta una vez, con los argumentos.
 */

#include "vx/parser.h"

namespace vx {

ast::ConceptRef Parser::parse_concept_ref() {
    ast::ConceptRef ref;
    ref.loc = current_.loc;
    // Opcionalmente cualificado (`mat.Numerico`).
    std::string name = consume().lexeme;
    while (current_.kind == TokenKind::DOT) {
        (void)consume(); // '.'
        if (current_.kind != TokenKind::IDENTIFIER) break;
        name += "." + consume().lexeme;
    }
    ref.name = util::InternedName::intern(name);
    /* Los argumentos del concepto, sin el primero: ese es el tipo al que se
     * aplica (`<V: View<i64>>` es `View<V, i64>`; `struct S : View<i64>` es
     * `View<S, i64>`). */
    if (current_.kind == TokenKind::LT) {
        (void)consume(); // '<'
        while (current_.kind != TokenKind::GT &&
               current_.kind != TokenKind::SHR &&
               current_.kind != TokenKind::END_OF_FILE) {
            auto arg = parse_type_node();
            if (!arg) break;
            ref.args.push_back(std::shared_ptr<ast::TypeNode>(std::move(arg)));
            if (!match(TokenKind::COMMA)) break;
        }
        (void)expect_close_angle(
            "se esperaba '>' al cerrar los argumentos del concepto");
    }
    return ref;
}

void Parser::parse_bound_concepts(ast::TypeBound &tb) {
    while (current_.kind == TokenKind::IDENTIFIER) {
        tb.concepts.push_back(parse_concept_ref());
        if (current_.kind != TokenKind::PLUS) break;
        (void)consume(); // '+' : otro concepto exigido
    }
}

} // namespace vx
