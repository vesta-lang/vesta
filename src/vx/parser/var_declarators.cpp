/**
 * @file var_declarators.cpp
 * @brief Declaraciones de variable: el tipo y los nombres que declara.
 *
 * Una declaracion puede nombrar VARIAS variables (`f64 dx = a, dy = b;`).  En
 * el arbol queda una `VarDeclStmt` por nombre, seguidas, para que nadie que lo
 * recorra tenga que saber que se escribieron juntas.  Lo que se escribe UNA
 * vez para todas -- el tipo y el almacenamiento -- se reparte aqui.
 *
 * El tipo delante vale para todos; con `auto` cada nombre infiere EL SUYO, que
 * es lo que deja declarar un contador y una lambda en el mismo `for`.
 *
 * @see vx/parser.h
 */
#include "vx/parser.h"

#include "vx/generics/generic_clone.h" // el tipo de cada nombre, clonado entero

#include <utility>

namespace vx {

namespace {

/**
 * @brief Si una palabra es la que pide inferir el tipo (`auto`, `var`).
 *
 * No son palabras reservadas: una variable puede llamarse `auto`.  Solo
 * significan "inferir" en la posicion del tipo, y eso lo decide quien llama.
 *
 * @param word La palabra.
 * @return true si es una de las dos.
 */
bool is_infer_word(const std::string &word) {
    return word == "auto" || word == "var";
}

/**
 * @brief Pasa a @p to el ALMACENAMIENTO de @p from: lo que se escribe una vez
 *        para todos los nombres de una declaracion.
 *
 * Es la lista de @c VarDeclStmt que no es de un nombre concreto; un campo
 * nuevo de esa clase va tambien aqui (ver el comentario de @c VarDeclStmt).
 * El tipo no esta: cada nombre lleva su clon, con sus propias dimensiones.
 *
 * @param from El primer nombre.
 * @param to   Uno de los siguientes.
 */
void copy_decl_storage(const ast::VarDeclStmt &from, ast::VarDeclStmt &to) {
    to.is_const = from.is_const;
    to.dir = from.dir;
    to.is_comptime = from.is_comptime;
    to.infer_type = from.infer_type;
    to.is_shared = from.is_shared;
    to.reg_binding = from.reg_binding;
    to.is_static = from.is_static;
}

} // namespace

bool Parser::is_inferred_decl_type_(const ast::TypeNode *t) const {
    if (!t || t->kind != ast::NodeKind::NamedTypeNode) return false;
    const auto *n = static_cast<const ast::NamedTypeNode *>(t);
    return n->type_args.empty() && is_infer_word(n->name);
}

void Parser::parse_decl_type_(ast::VarDeclStmt &vd) {
    /* `auto NAME = init;` o `var NAME = init;` -- inferencia local de tipo
     * desde el init.  `auto`/`var` se reconocen como IDENTIFIER contextual
     * seguido de OTRO IDENTIFIER (el nombre); asi NO se reservan como
     * keywords y el codigo con variables llamadas `auto`/`var` sigue
     * funcionando salvo en posicion de tipo en una declaracion. */
    if (current_.kind == TokenKind::IDENTIFIER &&
        is_infer_word(current_.lexeme) &&
        lex_.peek_at(0).kind == TokenKind::IDENTIFIER) {
        (void)consume(); // 'auto' o 'var'
        vd.type = nullptr;
        vd.infer_type = true;
        return;
    }
    vd.type = parse_type_node();
}

bool Parser::parse_declarator_(ast::VarDeclStmt &vd, bool name_read) {
    // Puntero a funcion estilo C como variable: `R (*name)(params) = init;`.
    bool got_fp_name = false;
    if (!name_read && vd.type) {
        std::string fp_name;
        std::unique_ptr<ast::TypeNode> fp_type;
        if (try_parse_c_func_ptr_(vd.type, fp_name, fp_type)) {
            vd.type = std::move(fp_type);
            vd.name = std::move(fp_name);
            got_fp_name = true;
        }
    }
    // azucar: `T !!name = init;` equivale a `nonnull T name = !!init;`.  El
    // `!!` entre tipo y nombre marca el tipo como no-null y envuelve el
    // inicializador con unwrap para insertar el check runtime + assert
    // compile-time.
    bool inline_nonnull = false;
    if (!name_read && !got_fp_name &&
        current_.kind == TokenKind::BANG_BANG) {
        inline_nonnull = true;
        (void)consume();
        if (vd.type) vd.type->is_nonnull = true;
    }
    if (!name_read && !got_fp_name) {
        if (!is_name_token(current_.kind)) {
            error_expected_name("nombre de variable",
                                "se esperaba un nombre tras el tipo");
            return false;
        }
        vd.name = consume().lexeme;
    }
    // Sintaxis C-style: `T name[N]` -> wrappear el tipo base en
    // ArrayTypeNode(N).  Acepta tambien `T name[]` (sin tamano, tipico de
    // parametros con decay-to-ptr).  Cadena permitida para matrices:
    // `T name[N][M]`.
    //
    // Para matrices `T name[N][M][K]` la dimension MAS A LA IZQUIERDA es la
    // EXTERIOR (igual que C): array de N de array de M de array de K de T.
    // El wrap ingenuo (cada bracket envuelve al previo) invierte el orden y
    // produce T[K][M][N]; por eso se recogen los tamanos y se envuelve de
    // DERECHA a IZQUIERDA.
    if (current_.kind == TokenKind::LBRACKET) {
        std::vector<std::pair<SourceLoc, std::unique_ptr<ast::Expr>>> dims;
        while (current_.kind == TokenKind::LBRACKET) {
            const SourceLoc abr_loc = current_.loc;
            (void)consume(); // '['
            std::unique_ptr<ast::Expr> sz;
            if (current_.kind != TokenKind::RBRACKET) sz = parse_expr();
            (void)expect(TokenKind::RBRACKET,
                         "se esperaba ']' al cerrar el tamano del array");
            dims.emplace_back(abr_loc, std::move(sz));
        }
        for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
            auto an = std::make_unique<ast::ArrayTypeNode>();
            an->loc = it->first;
            an->element_type = std::move(vd.type);
            an->size_expr = std::move(it->second);
            vd.type = std::move(an);
        }
    }
    if (match(TokenKind::ASSIGN)) {
        vd.init = parse_expr();
        // Con `T !!name = init` el init se envuelve en un `!!` automatico
        // para que el runtime falle pronto si resulta null.  Si ya se
        // escribio `!!init`, el doble unwrap es idempotente.
        if (inline_nonnull && vd.init) {
            auto un = std::make_unique<ast::UnaryExpr>();
            un->loc = vd.init->loc;
            un->op = ast::UnOp::Unwrap;
            un->operand = std::move(vd.init);
            vd.init = std::move(un);
        }
    }
    return true;
}

void Parser::parse_more_declarators_(
    const ast::VarDeclStmt &first, const ast::TypeNode *base,
    std::vector<std::unique_ptr<ast::Stmt>> &out) {
    while (current_.kind == TokenKind::COMMA) {
        (void)consume(); // ','
        auto vd = std::make_unique<ast::VarDeclStmt>();
        vd->loc = current_.loc;
        /* El almacenamiento se copia YA -- el `for` no lo toca despues -- y
         * otra vez al entregarlos, por si quien llamo a parse_var_decl_stmt
         * le puso al primero `static` o `comptime` al volver. */
        copy_decl_storage(first, *vd);
        vd->type = vxgen::clone_type_with_subst(base);
        if (!parse_declarator_(*vd, /*name_read=*/false)) return;
        out.push_back(std::move(vd));
    }
}

void Parser::take_pending_declarators_(
    const ast::Stmt *first, std::vector<std::unique_ptr<ast::Stmt>> &rest) {
    rest = std::move(pending_declarators_);
    pending_declarators_.clear();
    if (rest.empty()) return;
    /* Solo parse_var_decl_stmt los deja, justo antes de devolver su primer
     * nombre, y quien la llama devuelve ESE nodo: otra cosa aqui es un
     * fallo del propio parser, y se dice. */
    if (!first || first->kind != ast::NodeKind::VarDeclStmt) {
        error_here("internal parser error: extra declarators without the "
                   "declaration they belong to");
        rest.clear();
        return;
    }
    const auto &head = static_cast<const ast::VarDeclStmt &>(*first);
    for (auto &s : rest)
        copy_decl_storage(head, static_cast<ast::VarDeclStmt &>(*s));
}

std::unique_ptr<ast::Stmt> Parser::parse_var_decl_stmt(bool is_const,
                                                       bool from_comptime) {
    auto vd = std::make_unique<ast::VarDeclStmt>();
    vd->loc = current_.loc;
    vd->is_const = is_const;
    /* Direccion: `in i64* vista = null;`.  El MISMO lector que en un
     * parametro, para que la marca no signifique una cosa aqui y otra alli.
     * Lo que cambia es lo que queda de ella: en un parametro dice ademas que
     * hace la funcion; en una variable solo el permiso. */
    vd->dir = parse_opt_param_dir_();
    /* Storage-class `register("reg")` antes del tipo.  El patron ya lo
     * valido looks_like_register_storage() en el router, pero KW_CONST
     * tambien llama aqui; se reconsume de forma defensiva solo cuando el
     * patron `register ( "reg" )` aparece literalmente. */
    if (current_.kind == TokenKind::IDENTIFIER &&
        current_.lexeme == "register" &&
        lex_.peek_at(0).kind == TokenKind::LPAREN &&
        lex_.peek_at(1).kind == TokenKind::STRING_LIT &&
        lex_.peek_at(2).kind == TokenKind::RPAREN) {
        (void)consume();                    // 'register'
        (void)consume();                    // '('
        vd->reg_binding = current_.str_val; // nombre del registro
        (void)consume();                    // STRING_LIT
        (void)expect(TokenKind::RPAREN,
                     "se esperaba ')' tras register(\"reg\")");
    }
    /* El modificador `shared` marca el storage class.  Si tras `shared` viene
     * `<` NO es modificador (es el smart pointer `shared<T>`); si viene
     * cualquier otro comienzo de tipo, SI lo es y se consume, y
     * parse_type_node ve el tipo "limpio". */
    if (current_.kind == TokenKind::KW_SHARED &&
        lex_.peek_at(0).kind != TokenKind::LT) {
        (void)consume(); // modificador 'shared'
        vd->is_shared = true;
    }
    parse_decl_type_(*vd);
    // const-correctness C-style: un `const` LIDER sobre un tipo PUNTERO
    // qualifica el APUNTADO (`const char *p` = puntero a const char, puntero
    // MUTABLE), no el binding.  Sobre un tipo no-puntero, `const` sigue siendo
    // binding const.  No aplica a comptime (su `const` es "compile-time").
    if (is_const && !from_comptime && vd->type &&
        vd->type->kind == ast::NodeKind::PointerTypeNode) {
        ast::TypeNode *inner = vd->type.get();
        while (inner->kind == ast::NodeKind::PointerTypeNode)
            inner = static_cast<ast::PointerTypeNode *>(inner)->pointee.get();
        if (inner) inner->is_const = true;
        vd->is_const = false; // el puntero/binding es mutable (C)
    }
    /* El tipo de los demas nombres es el ESCRITO, antes de las dimensiones y
     * del puntero a funcion de este: `i64 a[4], b` declara un array y un
     * `i64`, como en C. */
    std::unique_ptr<ast::TypeNode> base =
        vxgen::clone_type_with_subst(vd->type.get());
    if (!parse_declarator_(*vd, /*name_read=*/false)) return nullptr;
    std::vector<std::unique_ptr<ast::Stmt>> more;
    parse_more_declarators_(*vd, base.get(), more);
    (void)expect(TokenKind::SEMICOLON,
                 "se esperaba ';' al final de la declaracion");
    for (auto &m : more)
        pending_declarators_.push_back(std::move(m));
    return vd;
}

} // namespace vx
