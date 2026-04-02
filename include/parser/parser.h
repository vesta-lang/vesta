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

#include <string>
#include <optional>
#include <charconv>
#include <cctype>
#include <algorithm>
#include <limits>
#include <system_error>
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
    /**
     * @brief Elimina '_' de la cadena (se usan como separadores visuales).
     */
    static std::string remove_underscores(std::string s) {
        s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
        return s;
    }

    /**
     * @brief Comprueba si la cadena s es un número válido en los formatos soportados.
     *
     * Soporta:
     *  - Hexadecimal: 0x... o 0X...
     *  - Binario: 0b... o 0B...
     *  - Octal: empieza por 0 (y no es 0x/0b)
     *  - Decimal: por defecto
     *
     * Acepta '+' inicial; no acepta '-' (devuelve false).
     * Permite '_' como separador de dígitos (se ignoran).
     */
    static bool is_valid_number(const std::string &s_in) {
        if (s_in.empty()) return false;
        std::string s = remove_underscores(s_in);

        size_t pos = 0;
        // '+' inicial opcional
        if (s[pos] == '+') ++pos;
        if (pos >= s.size()) return false;
        if (s[pos] == '-') return false; // no negativos para uint

        // detect prefix
        if (s.size() - pos >= 2 && s[pos] == '0' && (s[pos + 1] == 'x' || s[pos + 1] == 'X')) {
            // hexadecimal: debe tener al menos un dígito hexadecimal después del prefijo.
            if (pos + 2 >= s.size()) return false;
            for (size_t i = pos + 2; i < s.size(); ++i) {
                char c = s[i];
                if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
            }
            return true;
        }
        if (s.size() - pos >= 2 && s[pos] == '0' && (s[pos + 1] == 'b' || s[pos + 1] == 'B')) {
            // binary
            if (pos + 2 >= s.size()) return false;
            for (size_t i = pos + 2; i < s.size(); ++i) {
                char c = s[i];
                if (c != '0' && c != '1') return false;
            }
            return true;
        }
        // octal si comienza con '0' y longitud > 1 (y no hexadecimal/binario)
        if (s[pos] == '0' && (s.size() - pos) > 1) {
            for (size_t i = pos + 1; i < s.size(); ++i) {
                char c = s[i];
                if (c < '0' || c > '7') return false;
            }
            return true;
        }
        // decimal: all digits
        for (size_t i = pos; i < s.size(); ++i) {
            char c = s[i];
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    }

    /**
     * @brief Parsea la cadena y devuelve uint64_t si es válida y no desborda.
     *
     * @code{.cpp}
     * auto v1 = parse_number_safe("0xFF");        // 255
     * auto v2 = parse_number_safe("0b1010");      // 10
     * auto v3 = parse_number_safe("0755");        // octal -> 493
     * auto v4 = parse_number_safe("12345");       // decimal -> 12345
     * auto v5 = parse_number_safe("+1_000_000");  // 1000000 (guiones permitidos)
     * auto v6 = parse_number_safe("-10");         // std::nullopt (negativo no permitido)
     * auto ok = is_valid_number("0x1G");          // false (G no es hex)
     * @endcode
     *
     * @return std::optional<uint64_t> con el valor o std::nullopt en caso de error.
     */
    static std::optional<uint64_t> parse_number_safe(const std::string &s_in) {
        if (s_in.empty()) return std::nullopt;
        std::string s = remove_underscores(s_in);

        size_t pos = 0;
        bool has_plus = false;
        if (s[pos] == '+') {
            has_plus = true;
            ++pos;
        }
        if (pos >= s.size()) return std::nullopt;
        if (s[pos] == '-') return std::nullopt; // no negativos

        // hex
        if (s.size() - pos >= 2 && s[pos] == '0' && (s[pos + 1] == 'x' || s[pos + 1] == 'X')) {
            std::string body = s.substr(pos + 2);
            if (body.empty()) return std::nullopt;
            // use from_chars for hex (C++17/20)
            uint64_t val = 0;
            auto [ptr, ec] = std::from_chars(body.data(), body.data() + body.size(), val, 16);
            if (ec == std::errc() && ptr == body.data() + body.size()) return val;
            return std::nullopt;
        }

        // binario (sin from_chars base 2), analizar manualmente con comprobación de desbordamiento
        if (s.size() - pos >= 2 && s[pos] == '0' && (s[pos + 1] == 'b' || s[pos + 1] == 'B')) {
            std::string body = s.substr(pos + 2);
            if (body.empty()) return std::nullopt;
            uint64_t val = 0;
            for (char c: body) {
                if (c != '0' && c != '1') return std::nullopt;
                int bit = c - '0';
                if (val > (std::numeric_limits<uint64_t>::max() >> 1)) return std::nullopt; // overflow on shift
                val = (val << 1) | bit;
            }
            return val;
        }

        // octal (cero inicial y longitud > 1)
        if (s[pos] == '0' && (s.size() - pos) > 1) {
            std::string body = s.substr(pos + 1);
            uint64_t val = 0;
            for (char c: body) {
                if (c < '0' || c > '7') return std::nullopt;
                int d = c - '0';
                if (val > (std::numeric_limits<uint64_t>::max() >> 3)) return std::nullopt;
                val = (val << 3) + d;
            }
            return val;
        }

        // decimal
        {
            std::string body = s.substr(pos);
            if (body.empty()) return std::nullopt;
            uint64_t val = 0;
            for (char c: body) {
                if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
                int d = c - '0';
                if (val > (std::numeric_limits<uint64_t>::max() - d) / 10) return std::nullopt;
                val = val * 10 + d;
            }
            return val;
        }
    }

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
        int line, column;
        TokenType expected, found;
        std::string token_info;

        ParseError(int l, int c, const std::string &msg)
            : std::runtime_error(msg), line(l), column(c) {
        }

        ParseError(int l, int c, TokenType exp, TokenType f, const std::string &tok)
            : std::runtime_error("Expected " + token_type_to_string(exp) +
                                 ", found " + token_type_to_string(f)),
              line(l), column(c), expected(exp), found(f), token_info(tok) {
        }
    };

    class ParseWarning : public std::runtime_error {
    public:
        int line, column;
        std::string suggestion;

        ParseWarning(int l, int c, const std::string &msg, const std::string &sugg = "")
            : std::runtime_error(msg), line(l), column(c), suggestion(sugg) {
        }
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
        vm::Lexer &lexer; ///< Lexer fuente de tokens
        vm::Token current{vm::TokenType::EndOfFile, "", 0, 0}; ///< Token actual (lookahead)

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

        std::vector<std::unique_ptr<ASTNode> > parse_in_label();

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
