/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file parser.h
 * @brief Parser recursivo descendente para el lenguaje Vex.
 *
 * Estrategia:
 *  - Declaraciones, statements y tipos: descenso recursivo clasico.
 *  - Expresiones: cascada de niveles de precedencia (estilo C),
 *    cada nivel implementado como funcion propia.  Esto evita el
 *    coste de hashing/dispatch de un esquema Pratt generico y produce
 *    un binario mas predecible para el branch predictor.
 *  - Errores: sin excepciones.  Los errores se acumulan en Diagnostics
 *    y el parser intenta sincronizar al siguiente ";" o "}" para
 *    seguir reportando.
 *
 * Reglas de precedencia (de menor a mayor):
 *   1.  asignacion         = += -= *= /= %= &= |= ^= <<= >>=  (derecha-asoc)
 *   2.  or logico          ||
 *   3.  and logico         &&
 *   4.  or bit             |
 *   5.  xor bit            ^
 *   6.  and bit            &
 *   7.  igualdad           == !=
 *   8.  relacional         < <= > >=
 *   9.  shift              << >>
 *   10. aditivo            + -
 *   11. multiplicativo     * / %
 *   12. unario prefijo     ! ~ - + ++ --
 *   13. postfijo           ()  ++  --  []  .  ?.  ?.[ ]  ?
 *   14. primaria           literales, identificadores, ( expr )
 */

#ifndef VEX_PARSER_H
#define VEX_PARSER_H

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vex/ast.h"
#include "vex/diagnostic.h"
#include "vex/lexer.h"

namespace vex {

/// Fija el TARGET (os, arch) contra el que @c @Target evalua sus atomos
/// `os:`/`arch:` en compilacion AOT cross-target (el binario puede generarse
/// para un os/arch distinto del host de build).  Llamar ANTES de compilar.
/// Strings vacios => usar el host de build (comportamiento normal).
/// os: "windows"/"linux"/"macos".  arch: "x86_64"/"x86"/"arm64".
void set_aot_condcomp_target(const std::string &os,
                             const std::string &arch) noexcept;

/// Lee el override actual del target de @Target (el thread_local).  Usado por
/// el compile paralelo (M8) para propagar el target a los workers, que de otro
/// modo parsearian las variantes @Target contra el host (HALLAZGO-2).
void get_aot_condcomp_target(std::string &os, std::string &arch) noexcept;

/**
 * @class Parser
 * @brief Construye un AST a partir de los tokens producidos por @c Lexer.
 *
 * Es de un solo uso: una instancia produce un AST llamando a
 * @c parse_program() y luego se descarta.  Mantiene internamente el
 * token actual y acceso al lexer para producir el siguiente.
 */
class Parser {
  public:
    /**
     * @brief Construye el parser sobre un lexer y un sumidero de diagnosticos.
     *
     * @param lex   Lexer del que se consumen tokens.  Debe sobrevivir al
     * parser.
     * @param diags Sumidero de errores.  Debe sobrevivir al parser.
     */
    Parser(Lexer &lex, Diagnostics &diags);

    /**
     * @brief Phase M.L8: registra un alias de tipo conocido (typedef
     * importado de otro modulo via @c .vexi) ANTES de parsear.  El
     * @c looks_like_cast lo usa para reconocer @c (Name)expr como un
     * cast.  Idempotente: insertar el mismo nombre dos veces es no-op.
     */
    void add_known_alias(const std::string &name) {
        declared_aliases_.insert(name);
    }

    /**
     * @brief Parsea el programa completo desde el cursor actual del lexer.
     *
     * @return Un ModuleNode con todas las declaraciones top-level encontradas.
     *         Aun en caso de errores, devuelve un AST parcial; el caller debe
     *         consultar @c Diagnostics::has_errors() antes de bajar a IR.
     */
    std::unique_ptr<ast::ModuleNode> parse_program();

    /**
     * @brief parsea UNA expresion sobre el lexer actual.
     *
     * Util para macros estilo Lisp (`comptime_compile(str)`) que
     * construyen codigo a partir de un string en compile-time.  El
     * caller crea un Lexer sobre la cadena, instancia un Parser y
     * llama a este metodo.  Tras el parse el lexer queda en el token
     * que no consumio (tipicamente END_OF_FILE).  Si hay error
     * sintactico, se reporta via @c Diagnostics y devuelve nullptr.
     */
    std::unique_ptr<ast::Expr> parse_one_expr() { return parse_expr(); }

  private:
    // -----------------------------------------------------------------
    // Helpers de gestion de tokens.
    // -----------------------------------------------------------------

    /**
     * @brief Avanza al siguiente token y devuelve el consumido.
     */
    Token consume();

    /**
     * @brief Devuelve el token actual sin consumirlo.
     */
    const Token &peek() const noexcept { return current_; }

    /**
     * @brief @c true si el token actual es del tipo @p k.
     */
    [[nodiscard]] bool check(TokenKind k) const noexcept {
        return current_.kind == k;
    }

    /**
     * @brief Si el token actual es @p k, lo consume y devuelve true.
     */
    bool match(TokenKind k);

    /**
     * @brief Exige un token de tipo @p k.  Si no coincide, emite error y
     * devuelve false.
     *
     * El parser no aborta; intenta seguir parseando para detectar mas errores.
     *
     * @param k   Token esperado.
     * @param msg Mensaje a emitir cuando no coincide.
     * @return    Token consumido si coincidio, o un Token UNKNOWN si no.
     */
    Token expect(TokenKind k, const char *msg);

    /**
     * @brief Cierra un grupo de argumentos de tipo `<...>`.
     *
     * Acepta tanto un solo `>` (token GT) como el primer `>` de un
     * `>>` (token SHR), que se "parte" en dos: consume la mitad
     * izquierda y deja la derecha como nuevo current_ (kind=GT).
     * Esto permite tipos genericos anidados como
     * `VirtualPtr<VirtualPtr<i64>>` sin requerir espacios entre los
     * dos cierres (`> >`).
     */
    Token expect_close_angle(const char *msg);

    /**
     * @brief Avanza tokens hasta hallar un punto de sincronizacion.
     *
     * Sincronizadores: ';', '}', EOF, y el inicio de cualquier
     * keyword de declaracion top-level.  Usado tras un error para
     * limitar la cascada de mensajes.
     */
    void synchronize();

    /**
     * @brief Reporta un error en la posicion del token actual.
     */
    void error_here(const char *msg);

    /**
     * @brief Reporta un error en la posicion de un token dado.
     */
    void error_at(const Token &tok, const char *msg);

    // -----------------------------------------------------------------
    // Reglas gramaticales: top-level y declaraciones.
    // -----------------------------------------------------------------

    std::unique_ptr<ast::Node> parse_top_level_decl();

    /**
     * @brief parsea @c extern @c "lib.dll" @c { @c fn @c name(params) @c -> @c
     * R; @c ... @c }
     *
     * El bloque produce N decls (uno por funcion); el parser los empuja
     * directamente al modulo via @p mod.  Cada @c ExternFnDecl comparte
     * el campo @c lib del bloque.  Sin parseo de nombre de parametro
     * obligatorio: si no hay identificador tras el tipo, sintetiza
     * @c "__arg<i>" para que el resto del frontend pueda referirlo.
     * El tipo de retorno usa @c PrimitiveKind::VOID si no hay @c "->".
     */
    void parse_extern_block(ast::ModuleNode &mod);
    std::unique_ptr<ast::FunctionDecl>
    parse_function_decl(std::unique_ptr<ast::TypeNode> ret_type,
                        std::string name, SourceLoc loc);
    std::unique_ptr<ast::GlobalVarDecl>
    parse_global_var_decl(std::unique_ptr<ast::TypeNode> type, std::string name,
                          SourceLoc loc, bool is_const);
    std::unique_ptr<ast::ParamDecl> parse_param();
    std::unique_ptr<ast::TypeAliasDecl> parse_typedef_decl();
    std::unique_ptr<ast::TypeAliasDecl> parse_using_decl();
    /// @brief Parsea @c import "path" [as alias] [only A, B];
    /// @param is_public_reexport @c true si vino precedido de @c public.
    std::unique_ptr<ast::ImportDecl> parse_import_decl(bool is_public_reexport);
    /// @brief Parsea @c "namespace foo { decls }" (Phase M.7.c).
    std::unique_ptr<ast::NamespaceDecl> parse_namespace_decl();
    /// @c bytes name { db/dw/dd/dq/times ... }  (datos crudos NASM, AOT).
    std::unique_ptr<ast::BytesDecl> parse_bytes_decl();
    /// @c asm name { <nasm 16/32/64> }  (codigo ensamblado por Keystone, AOT).
    std::unique_ptr<ast::BytesDecl> parse_asm_block_decl();
    std::unique_ptr<ast::StructDecl> parse_struct_decl();
    /// `typedef struct {...} Name;` o
    /// `typedef enum {...} Name;`.  Devuelve StructDecl o EnumDecl.
    std::unique_ptr<ast::Node> parse_typedef_struct_or_enum();
    std::unique_ptr<ast::ClassDecl> parse_class_decl();
    std::unique_ptr<ast::ClassDecl> parse_interface_decl();

    /**
     * @briefparsea @c enum @c Name { @c Variant, @c Variant(T1, T2), ... }
     *
     * El parser reconoce las dos formas de variante:
     *   - Sin payload: solo el identificador de la variante.
     *   - Con payload: identificador seguido de @c (T1, T2, ...).
     * Las variantes se separan por coma; el bloque puede tener una
     * coma trailing opcional.  No se admite valor por defecto ni
     * herencia (los enums son tipos de datos algebraicos planos).
     */
    std::unique_ptr<ast::EnumDecl> parse_enum_decl();

    /**
     * @brief parsea @c match @c expr @c { @c case ... }
     *
     * Sintaxis aceptada:
     *   match expr {
     *       case Variant            => stmt;
     *       case Variant(b1, b2)    => { stmts; }
     *       case _                  => default_stmt;   // catchall opcional
     *   }
     * El cuerpo de cada arm puede ser una expression-bodied
     * (`=> expr;`) que se reescribe a `{ return expr; }` o un bloque
     * (`=> { ... }`).  Cada arm termina en @c ;
     */
    std::unique_ptr<ast::Expr> parse_match_expr();
    std::unique_ptr<ast::BlockStmt> parse_method_body(bool is_void);

    /// @brief Parsea `<U1, U2, ...>` de type-params de un metodo generico.
    ///
    /// Precondicion: @c current_ es @c LT.  Consume hasta el `>` de cierre
    /// y rellena @p out con los nombres de los parametros de tipo.  Se usa
    /// en las declaraciones de metodo (`R metodo<U>(...)`).  Para metodos
    /// NO genericos, el llamante no invoca este helper (no hay `<`).
    void parse_method_type_params(std::vector<std::string> &out);

    /// @brief Parsea `<T, U: C, V: A + B>`: type-params con constraints (#6).
    ///
    /// Precondicion: @c current_ es @c LT.  Rellena @p params con los
    /// nombres y @p bounds con los constraints inline (`T: C`).  Equivale a
    /// @c parse_method_type_params pero ademas captura los bounds `: ...`.
    void parse_type_params_with_bounds(std::vector<std::string> &params,
                                       std::vector<ast::TypeBound> &bounds);

    /// @brief Parsea una clausula `where T: A + B, U: C` (#6).
    ///
    /// Precondicion: @c current_ es el identificador contextual @c where.
    /// anyade los bounds a @p bounds (acumula con los inline si los hubo).
    void parse_where_clause(std::vector<ast::TypeBound> &bounds);

    /// @brief Parsea el patron `<i64>` / `<T*>` de una especializacion (#7).
    ///
    /// Precondicion: @c current_ es '<'.  Rellena @p pattern con los
    /// type-nodes del patron y @p fresh_params con los identificadores que
    /// aparecen DENTRO de un patron compuesto (`T*`, `T[]`) -> son los params
    /// frescos de una especializacion PARCIAL.  Un identificador desnudo
    /// (`Punto`) o un primitivo (`i64`) es CONCRETO (especializacion TOTAL).
    void
    parse_specialization_pattern(std::vector<std::unique_ptr<ast::TypeNode>> &pattern,
                                 std::vector<std::string> &fresh_params);

    /// #7: nombres de struct/clase/funcion genericos ya vistos como template
    /// PRIMARIO.  La PRIMERA decl `Caja<...>` es el primario; las siguientes
    /// con el mismo nombre son especializaciones (total/parcial).
    std::unordered_set<std::string> generic_struct_names_seen_;
    std::unordered_set<std::string> generic_class_names_seen_;
    std::unordered_set<std::string> generic_fn_names_seen_;

    /// @brief Parsea una declaracion de concepto (#6).  Tres formas:
    ///   - `concept N<T> = <bool-expr>;`        (predicado)
    ///   - `concept N<T> { <comptime stmts> }`  (bloque estilo comptime)
    ///   - `concept N { <metodo-sigs> }`        (estructural -> has_method)
    /// Precondicion: @c current_ es el identificador contextual @c concept.
    std::unique_ptr<ast::ConceptDecl> parse_concept_decl();

    /// @brief Registra @p names como type-aliases temporales para que
    /// `(T)x` se reconozca como cast dentro de un body generico (los
    /// type-params del struct/clase contenedor y del metodo no son
    /// typedefs conocidos a priori).  Devuelve SOLO los nombres que
    /// realmente se insertaron (no estaban ya en @c declared_aliases_),
    /// para retirarlos despues con @c unregister_temp_type_aliases.
    std::vector<std::string>
    register_temp_type_aliases(const std::vector<std::string> &names);
    /// @brief Retira los aliases temporales insertados por el helper
    /// anterior (restaura @c declared_aliases_ al estado previo).
    void unregister_temp_type_aliases(const std::vector<std::string> &inserted);

    // -----------------------------------------------------------------
    // Reglas gramaticales: tipos.
    // -----------------------------------------------------------------

    std::unique_ptr<ast::TypeNode> parse_type_node();

    // -----------------------------------------------------------------
    // Reglas gramaticales: statements.
    // -----------------------------------------------------------------

    std::unique_ptr<ast::BlockStmt> parse_block();
    std::unique_ptr<ast::Stmt> parse_statement();
    std::unique_ptr<ast::Stmt> parse_var_decl_stmt(bool is_const);
    std::unique_ptr<ast::Stmt> parse_if_stmt();
    std::unique_ptr<ast::Stmt> parse_while_stmt();
    std::unique_ptr<ast::Stmt> parse_do_while_stmt();
    std::unique_ptr<ast::Stmt> parse_for_stmt();
    std::unique_ptr<ast::Stmt> parse_return_stmt();
    std::unique_ptr<ast::Stmt> parse_try_stmt();
    std::unique_ptr<ast::Stmt> parse_throw_stmt();
    std::unique_ptr<ast::Stmt> parse_synchronized_stmt();
    std::unique_ptr<ast::Stmt>
    parse_asm_stmt(); ///< Phase AS: asm [quals] { ... } clobbers(...)
    std::unique_ptr<ast::Stmt> parse_expr_stmt();

    // -----------------------------------------------------------------
    // Reglas gramaticales: expresiones, por nivel de precedencia.
    // -----------------------------------------------------------------

    std::unique_ptr<ast::Expr> parse_expr();
    std::unique_ptr<ast::Expr> parse_assignment();
    std::unique_ptr<ast::Expr> parse_ternary(); ///< cond ? then : else
    std::unique_ptr<ast::Expr> parse_logical_or();
    std::unique_ptr<ast::Expr> parse_logical_and();
    std::unique_ptr<ast::Expr> parse_bitwise_or();
    std::unique_ptr<ast::Expr> parse_bitwise_xor();
    std::unique_ptr<ast::Expr> parse_bitwise_and();
    std::unique_ptr<ast::Expr> parse_equality();
    std::unique_ptr<ast::Expr> parse_relational();
    std::unique_ptr<ast::Expr> parse_shift();
    std::unique_ptr<ast::Expr> parse_additive();
    std::unique_ptr<ast::Expr> parse_multiplicative();
    std::unique_ptr<ast::Expr> parse_unary();
    std::unique_ptr<ast::Expr> parse_postfix();
    std::unique_ptr<ast::Expr> parse_primary();

    /**
     * @brief Parsea una lambda @c (args) => expr/{stmts} a partir del LPAREN.
     *
     * El @c current_ debe estar en LPAREN cuando se llama (precondicion
     * que comprueba @c is_lambda_start()).  Consume desde el LPAREN hasta
     * el final del cuerpo (expression-bodied o block).
     */
    std::unique_ptr<ast::Expr> parse_lambda_expr();

    /**
     * @brief Lookahead que determina si lo que sigue al @c current_ LPAREN
     *        es una lambda y no una expresion parentizada.
     *
     * Cuenta parentesis con balance hasta cerrar el grupo; si el siguiente
     * token tras el RPAREN matching es FAT_ARROW, es una lambda.  Sin
     * consumir tokens (usa @c Lexer::peek_at).  Coste O(N) donde N es la
     * longitud del grupo entre parentesis; despreciable para listas de
     * argumentos tipicas.
     *
     * @return true si la siguiente forma sintactica es una lambda.
     */
    [[nodiscard]] bool is_lambda_start() const noexcept;

    /**
     * @brief Decide si el token actual abre un tipo primitivo (i32, ...).
     */
    [[nodiscard]] bool starts_type() const noexcept;

    /**
     * @brief Phase AS inc.2: decide si el statement actual es un var-decl
     *        con storage-class @c register("reg").
     *
     * Reconoce el patron EXACTO @c register @c ( @c "reg" @c ) seguido de
     * un type-starter.  Solo asi se trata como var-decl; una llamada
     * @c register("x"); (sin tipo despues) queda como expresion.  El
     * lookahead es preciso para no robar sintaxis a expresiones legitimas.
     */
    [[nodiscard]] bool looks_like_register_storage() const noexcept;

    /**
     * @brief Decide si el `(` actual inicia un cast C-style `(T) expr`.
     *
     * El parser reconoce el patron `(<type>) <unary>` solo cuando el
     * primer token tras `(` es claramente un type-starter (primitivo,
     * @c VirtualPtr, @c fn, @c nonnull) y la secuencia se cierra con
     * un `)` seguido de un token que pueda iniciar una expresion.
     * Sin esto, el parser bajaria a `(expr)` y fallaria al ver el
     * `)` en mitad del tipo.  No aceptamos `(Foo) x` para typedefs
     * de identificadores; el usuario puede escribir
     * `(VirtualPtr<Foo>) x` o `(Foo*) x` que son inequivocos.
     */
    [[nodiscard]] bool looks_like_cast() const noexcept;

    Lexer &lex_;
    Diagnostics &diags_;
    Token current_;
    /// contador para nombres unicos de static_asserts top-level
    /// que se modelan como GlobalVarDecl dummy.  El type checker los
    /// procesa en la pasada de globales sin generar storage runtime.
    uint32_t static_assert_counter_ = 0;
    /// Registry de macros con params @c expr.  Llave: nombre del @Macro.
    /// Valor: indices (0-based) de los params declarados con tipo `expr`.
    /// Poblado cuando se parsea la FunctionDecl del macro; consultado
    /// en @c parse_postfix al construir un @c CallExpr para hacer
    /// raw-text capture en las posiciones marcadas.
    ///
    /// Limitacion Phase A: el @Macro debe estar declarado ANTES de su
    /// llamada en el archivo (single-pass parser). Forward refs requieren
    /// un pre-scanner (Phase B futura).
    std::unordered_map<std::string, std::vector<int>> macro_expr_params_;

    /// Set de identifiers declarados como typedef / using en el archivo
    /// (alias de tipos).  Poblado por @c parse_typedef_decl /
    /// @c parse_using_decl.  Consultado por @c looks_like_cast para
    /// permitir `(MyTypedef) x` cuando el identifier es un alias
    /// conocido (cierre de Item 19 de pendientes).  Limitacion
    /// single-pass: el typedef debe declararse ANTES del uso (igual
    /// que cualquier forward decl en C/Vex).
    std::unordered_set<std::string> declared_aliases_;

    /// Phase M.L24: flag indicando si la ultima invocacion de
    /// @c parse_top_level_decl skipeo la decl por @c @Target no
    /// matcheado.  @c parse_program lo consulta para evitar
    /// llamar a @c synchronize() (que descartaria tokens validos
    /// de la siguiente decl).
    bool last_decl_was_target_skip_ = false;

    /// @NoExceptions a nivel modulo (sticky): una vez visto, se propaga a
    /// ModuleNode::no_exceptions -> todas las funciones lo heredan.
    bool module_no_exceptions_ = false;

    /// Phase M6.a L.3: visibilidad pendiente capturada en
    /// @c parse_top_level_decl.  Los sub-parsers que produzcan un
    /// nodo top-level llaman @c apply_pending_visibility_ al final
    /// antes de devolver el nodo.  Valores: 0 = sin keyword (mantener
    /// el default del nodo), 1 = @c public explicito, 2 = @c private
    /// explicito.  El destructor @c VisGuard en
    /// @c parse_top_level_decl resetea a 0 al salir.
    uint8_t pending_visibility_ = 0;

  public:
    /// Helper que aplica @c pending_visibility_ al nodo si el nodo
    /// es de un kind con campo @c is_public.  Llamado al final de
    /// cada @c parse_<xxx>_decl que pueda aparecer en top-level.
    /// No-op si @c pending_visibility_ == 0.
    void apply_pending_visibility(ast::Node *n) noexcept;

    /// Phase M.L24: descarta una decl top-level cuya @Target no matcheo.
    /// Consume tokens hasta el final natural de la decl ({ ... } o ;).
    void skip_target_skipped_decl();
};

} // namespace vex

#endif // VEX_PARSER_H
