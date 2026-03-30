/**
 * VestaVM - Máquina Virtual Distribuida
 * 
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 * 
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 * 
 * Descargo: Autor no responsable por modificaciones.
 * @file parser.h
 * @brief Parser recursivo descendente para el lenguaje VMProject.
 *
 * Convierte secuencias de tokens (generados por Lexer) en un árbol de sintaxis abstracta (AST).
 * Implementa un parser predictivo LL(1) usando Recursive Descent, ideal para la gramática simple
 *
 * Copyright (c) 2026 David López T.
 * Proyecto VMProject - Licencia MIT
 */

#ifndef PARSER_H
#define PARSER_H

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

#include "ast.h"
#include "lexer/lexer.h"

/**
 * @namespace vm
 * @brief Namespace principal del VMProject.
 *
 * Contiene lexer, parser, AST y componentes
 */
namespace vm {
    static uint64_t parse_number(const std::string &s) {
        // Hexadecimal
        if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0)
            return std::stoull(s, nullptr, 16);

        // Binario
        if (s.rfind("0b", 0) == 0 || s.rfind("0B", 0) == 0)
            return std::stoull(s.substr(2), nullptr, 2);

        // Octal
        if (s.size() > 1 && s[0] == '0')
            return std::stoull(s, nullptr, 8);

        // Decimal
        return std::stoull(s, nullptr, 10);
    }

    enum class OpArity { ZERO, ONE, TWO };

    /**
     * @struct InstructionPattern
     * @brief Patrón de instrucción para el InstructionSet del parser.
     *
     * Define las propiedades de una instrucción de la VM para parsing automático:
     * - Número de operandos requeridos (0, 1, 2)
     * - Aliases opcionales (sinónimos soportados)
     *
     * **Ejemplo uso:**
     * ```
     * {"mov", {"mov", OpArity::TWO, {"move", "MOV", "Move"}}}
     * ```
     * Soporta: `mov`, `MOV`, `move`, `Move`
     */
    struct InstructionPattern {
        /**
         * @field opcode
         * @brief Nombre **canónico** de la instrucción (siempre minúsculas).
         *
         * Usado para:
         * - Codegen (emitir bytes)
         * - Debugging (print AST)
         * - Serialización
         *
         * **Ejemplo:** `"mov"`, `"add"`, `"jmp"` (nunca `"MOV"`)
         */
        std::string opcode;

        /**
         * @field arity
         * @brief **Aridad** - Número de operandos requeridos.
         *
         * Controla parsing automático:
         * - `OpArity::ZERO` -> `nop`, `hlt` (sin operandos)
         * - `OpArity::ONE` -> `jmp label`, `push r0` (1 operando)
         * - `OpArity::TWO` -> `mov r0, 1` (2 operandos)
         *
         * **Codegen bytes:**
         * ```
         * ZERO -> 1 byte opcode
         * ONE  -> 1 + 4 bytes (addr/label)
         * TWO  -> 1 + 4 reg + 4 imm
         * ```
         */
        OpArity arity;

        /**
         * @field aliases
         * @brief Lista de **sinónimos**
         *
         * **OPCIONAL** - Si vacío, solo `opcode` funciona.
         *
         * **Ejemplos:**
         * ```
         * mov: ["move", "MOV", "Move"] -> mov r0,1 / MOVE r0,1
         * add: ["sum", "ADD"] -> add r0,1 / SUM r0,1
         * jmp: [] -> SOLO jmp label
         * ```
         */
        std::vector<std::string> aliases; // "mov", "move"
    };



    /**
     *
     * @param type espera un token de tipo decimal, hexadecimal, octal o binario
     * @return si es uno de los mencionados, devuelve true.
     */
    inline bool is_number_token(TokenType type) {
        return type == TokenType::NUMBER_DEC || type == TokenType::NUMBER_HEX ||
                type == TokenType::NUMBER_BIN || type == TokenType::NUMBER_OCT;
    }

    class ParseError : public std::runtime_error {
    public:
        int         line,     column;
        TokenType   expected, found;
        std::string token_info;

        ParseError(int l, int c, const std::string &msg)
            : std::runtime_error(msg), line(l), column(c) {}

        ParseError(int l, int c, TokenType exp, TokenType f, const std::string &tok)
            : std::runtime_error("Expected " + token_type_to_string(exp) +
                  ", found " + token_type_to_string(f)),
              line(l), column(c), expected(exp), found(f), token_info(tok) {}
    };

    class ParseWarning : public std::runtime_error {
    public:
        int         line, column;
        std::string suggestion;

        ParseWarning(int l, int c, const std::string &msg, const std::string &sugg = "")
            : std::runtime_error(msg), line(l), column(c), suggestion(sugg) {}
    };



    /**
     * @class Parser
     * @brief Parser recursivo descendente (top-down) para el lenguaje VMProject.
     *
     * Implementa un parser predictivo LL(1) que consume tokens del Lexer y genera un AST.
     * Soporta la gramática:
     *
     * ```
     * program     ::= section*
     * section     ::= "code:" statement* "end_code:" | "data:" data_decl* "end_data:"
     * statement   ::= IDENTIFIER REGISTER COMMA NUMBER | ...
     * data_decl   ::= IDENTIFIER "db" STRING
     * ```
     *
     * **Características:**
     * - Manejo de errores con línea/columna exacta
     * - Recuperación de errores (continúa parseando)
     * - Fácil extensión para nuevas construcciones sintácticas
     * - Zero-copy donde posible (referencias a lexemes originales)
     */
    class Parser {
    private:
        vm::Lexer &lexer;                                       ///< Lexer fuente de tokens
        vm::Token  current{vm::TokenType::EndOfFile, "", 0, 0}; ///< Token actual (lookahead)

        /**
         * @brief Avanza al siguiente token del lexer.
         *
         * Actualiza `current` con el siguiente token disponible.
         */
        void advance() {
            current = lexer.next_token();
        }

        [[nodiscard]] Token peek() const {
            return lexer.peek_token();
        }

        /**
         * @brief Intenta consumir un token de tipo específico.
         * @param type Tipo de token esperado
         * @return `true` si el token actual coincide y se consume, `false` si no
         */
        bool match(TokenType type);

        /**
         * @brief Consume un token específico o reporta error.
         * @param type Tipo de token requerido
         * @param msg Mensaje de error descriptivo
         * @return `true` si éxito, `false` si error
         */
        bool expect(TokenType type, const std::string &msg);


        /**
         * @brief Reporta un error de parsing con contexto.
         * @param tok Token donde ocurrió el error
         * @param msg Mensaje descriptivo del error
         */
        void error(const Token &tok, const std::string &msg);

        void warning(int line, int col, const std::string &msg, const std::string &sugg);

        void print_warnings() const;

        std::unique_ptr<AnnotationNode> parse_annotation();

        std::unique_ptr<ExprNode> parse_expression();

        std::unique_ptr<ExprNode> parse_term();

        std::unique_ptr<ExprNode> parse_factor();

    public:
        std::vector<ParseWarning> warnings; ///< Acumulador warnings

        /**
         * @brief Constructor del parser.
         * @param l Referencia al lexer que provee tokens
         *
         * Inicializa el parser y consume el primer token.
         */
        explicit Parser(Lexer &l) : lexer(l) {
            advance();
        }

        /**
         * @brief Parsea el programa completo.
         * @return Árbol de secciones parseadas (AST raíz)
         *
         * Consume tokens hasta `EndOfFile` generando el AST completo del programa.
         */
        std::vector<std::unique_ptr<ASTNode> > parse();

        std::unique_ptr<ASTNode> parse_mem_term();

        std::unique_ptr<ASTNode> parse_mem_expression();

        std::unique_ptr<ASTNode> parse_mem_factor();

        std::vector<std::unique_ptr<ASTNode>> parse_in_label();

        std::unique_ptr<ASTNode> parser_end_label();

        /**
         * @brief Parsea una sección (`code:`, `data:`, etc.).
         * @return Nodo AST de la sección o `nullptr` si no es sección válida
         */
        std::unique_ptr<ASTNode> parse_section();

        std::unique_ptr<ASTNode> parse_operand();

        std::unique_ptr<ASTNode> parse_data_directive();

        std::unique_ptr<ASTNode> parse_instruction();

        /**
         * Al encontrar un import, el lexer y parser analizaran este por otra parte
         * @return nodo de timpo importacion que indica el archivo importado
         */
        std::unique_ptr<ASTNode> parse_import();

        /**
         *
         * @param type tipo de token esperado, si es el token esperado, avanza, sino, muestra error sin avanzar.
         * @param msg mensaje de error
         * @return devuelve el token esperado, o el que se encontro
         */
        inline Token expectToken(TokenType type, const std::string &msg) {
            if (current.type == type) {
                return current;
            }
            error(current, msg);
            return current;
        }

        /**
         * @brief Parsea un statement de código (`mov r0, 1`).
         * @return Nodo `RegisterAssign` u otro statement o `nullptr`
         */
        std::unique_ptr<ASTNode> parse_statement();
    };
}


#endif //PARSER_H
