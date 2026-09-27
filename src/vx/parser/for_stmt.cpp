/**
 * @file for_stmt.cpp
 * @brief El `for`: el de contador (`init; cond; pasos`) y el de coleccion
 *        (`T x : col`).
 *
 * El inicializador y los pasos son LISTAS separadas por coma:
 * `for (auto i = 0, next = f; i < n; i = next(i), k++)`.  Solo aqui: la coma
 * del lenguaje no es un operador, y fuera del `for` separa argumentos.
 *
 * El inicializador declara con el MISMO lector que una declaracion normal
 * (parser/var_declarators.cpp).  Antes lo armaba a mano y se dejaba cosas:
 * `for (auto i = 0; ...)` daba "unknown type 'auto'".
 *
 * @see vx/parser.h
 */
#include "vx/parser.h"

#include "vx/generics/generic_clone.h" // el tipo de cada nombre, clonado entero

#include <utility>

namespace vx {

void Parser::parse_for_tail_(ast::ForStmt &s) {
    if (current_.kind != TokenKind::SEMICOLON) s.cond = parse_expr();
    (void)expect(TokenKind::SEMICOLON,
                 "se esperaba ';' tras la condicion del 'for'");
    if (current_.kind != TokenKind::RPAREN) {
        s.step.push_back(parse_expr());
        while (match(TokenKind::COMMA))
            s.step.push_back(parse_expr());
    }
    (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar 'for'");
    s.body = parse_statement();
}

std::unique_ptr<ast::Stmt> Parser::parse_for_stmt() {
    const SourceLoc for_loc = current_.loc;
    // Aceptar tanto KW_FOR como IDENT("foreach") contextual.  Ambos
    // delegan al mismo handler que detecta automaticamente la
    // sintaxis foreach (`T x : col`) vs counted-for (`init; cond; step`).
    (void)consume(); // 'for' o 'foreach'
    (void)expect(TokenKind::LPAREN, "se esperaba '(' tras 'for'/'foreach'");

    // `comptime const/var T NAME = expr` como init del for: el contador es
    // un valor comptime dentro del cuerpo si el resto del for esta en
    // contexto comptime (p.ej. dentro del cuerpo de una comptime fn).
    bool init_is_comptime = false;
    bool init_is_comptime_const = false;
    if (current_.kind == TokenKind::IDENTIFIER &&
        current_.lexeme == "comptime") {
        Lexer &mut_lex = const_cast<Lexer &>(lex_);
        if (mut_lex.peek_at(0).kind == TokenKind::KW_CONST) {
            (void)consume(); // comptime
            (void)consume(); // const
            init_is_comptime = true;
            init_is_comptime_const = true;
        } else if (mut_lex.peek_at(0).kind == TokenKind::IDENTIFIER &&
                   mut_lex.peek_at(0).lexeme == "var") {
            (void)consume(); // comptime
            (void)consume(); // var
            init_is_comptime = true;
        }
    }

    // Disambiguacion entre foreach y counted-for.
    //   foreach: `for (T NAME : EXPR) body`
    //   counted: `for (init? ; cond? ; step?) body`
    // Ambos empiezan con un tipo opcional; la diferencia es lo que viene tras
    // el primer identificador, y los tokens no se pueden devolver: por eso el
    // tipo y el nombre se leen aqui y el resto del declarador despues.
    if (starts_type()) {
        auto type_node = parse_type_node();
        if (!type_node) {
            error_here("tipo invalido en for");
            return nullptr;
        }
        if (current_.kind != TokenKind::IDENTIFIER) {
            error_expected_name(
                "nombre de variable del 'for'",
                "se esperaba un identificador tras el tipo en for");
            return nullptr;
        }
        std::string name = consume().lexeme;
        if (current_.kind == TokenKind::COLON) {
            (void)consume(); // ':'
            auto fe = std::make_unique<ast::ForEachStmt>();
            fe->loc = for_loc;
            fe->iter_type = std::move(type_node);
            fe->iter_name = std::move(name);
            fe->iter_expr = parse_expr();
            (void)expect(TokenKind::RPAREN, "se esperaba ')' tras for-each");
            fe->body = parse_statement();
            return fe;
        }
        auto s = std::make_unique<ast::ForStmt>();
        s->loc = for_loc;
        auto vd = std::make_unique<ast::VarDeclStmt>();
        vd->loc = for_loc;
        vd->name = std::move(name);
        vd->is_comptime = init_is_comptime;
        vd->is_const = init_is_comptime_const;
        /* `auto`/`var` delante del nombre se ha leido como un tipo con ese
         * nombre, porque aqui no se sabia aun que era una declaracion. */
        if (is_inferred_decl_type_(type_node.get()))
            vd->infer_type = true;
        else
            vd->type = std::move(type_node);
        std::unique_ptr<ast::TypeNode> base =
            vxgen::clone_type_with_subst(vd->type.get());
        (void)parse_declarator_(*vd, /*name_read=*/true);
        std::vector<std::unique_ptr<ast::Stmt>> more;
        parse_more_declarators_(*vd, base.get(), more);
        s->init.push_back(std::move(vd));
        for (auto &m : more)
            s->init.push_back(std::move(m));
        (void)expect(TokenKind::SEMICOLON,
                     "se esperaba ';' tras init del 'for'");
        parse_for_tail_(*s);
        return s;
    }

    // Sin tipo al inicio: el inicializador son expresiones (o nada).
    auto s = std::make_unique<ast::ForStmt>();
    s->loc = for_loc;
    if (!match(TokenKind::SEMICOLON)) {
        do {
            auto es = std::make_unique<ast::ExprStmt>();
            es->loc = current_.loc;
            es->expr = parse_expr();
            s->init.push_back(std::move(es));
        } while (match(TokenKind::COMMA));
        (void)expect(TokenKind::SEMICOLON,
                     "se esperaba ';' tras init del 'for'");
    }
    parse_for_tail_(*s);
    return s;
}

} // namespace vx
