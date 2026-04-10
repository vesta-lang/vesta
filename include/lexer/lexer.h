/*
 * VestaVM - Máquina Virtual Distribuida
 * 
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 * 
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 * 
 * Descargo: Autor no responsable por modificaciones.
 */

#ifndef LEXER_H
#define LEXER_H

#include <cctype>
#include <iostream>
#include <string>
#include <fstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vm {
    /**
     *
     * @param lexeme un lexerma que tenga formato de registro
     * @return si no es un registro, entonces devuelve false
     */
    inline bool is_register(const std::string &lexeme) {
        if (lexeme.empty()) return false;

        // Tabla de registros especiales
        static const std::unordered_set<std::string> special_regs = {
            "rip", "rbp", "rsp", "rflags",
            "cur0", "cur1", "cur2", "cur3"
        };

        if (special_regs.count(lexeme))
            return true;

        // Registros generales: r[0-15][b|w|d]?
        if (lexeme[0] == 'r') {
            size_t i = 1;

            // Leer número
            while (i < lexeme.size() && isdigit((unsigned char) lexeme[i]))
                i++;

            if (i == 1) return false; // no había número

            int reg_num = std::stoi(lexeme.substr(1, i - 1));
            if (reg_num < 0 || reg_num > 15)
                return false;

            // Sufijo opcional
            if (i == lexeme.size())
                return true;

            if (i + 1 == lexeme.size()) {
                char suf = lexeme[i];
                return (suf == 'b' || suf == 'w' || suf == 'd');
            }

            return false;
        }

        return false;
    }


    /**
     *
     * @param lexeme espera un lexema
     * @return devuelve si es una directiva de datos valida o invalida
     */
    inline bool is_data_directive(const std::string &lexeme) {
        return lexeme == "db" ||
                lexeme == "dw" ||
                lexeme == "dd" ||
                lexeme == "dq" ||
                lexeme == "ptr";
    }

    /**
     * @enum TokenType
     * @brief Tipos de tokens que el lexer puede reconocer.
     */
    enum class TokenType {
        // --- NUMERIC LITERALS ---
        NUMBER_HEX,   ///< Hexadecimal: 0xFF
        NUMBER_BIN,   ///< Binario: 0b101010
        NUMBER_OCT,   ///< Octal: 0o755
        NUMBER_FLOAT, ///< Numero flotante: 3.14
        NUMBER_DEC,   ///< Numero decimal: 12345

        // --- IDENTIFIERS ---
        IDENTIFIER,     ///< Identificador: variable, label, etc.
        DATA_DIRECTIVE, ///< Directiva de datos: db, dq, dw, dd, ptr
        REGISTER,       ///< Registro: rax, r1, sp, etc.

        // --- STRINGS ---
        CHAR,   ///< Literal de caracter: 'a', '\n'
        STRING, ///< Literal de string: "hola"

        // --- GROUPING SYMBOLS ---
        LPAREN,   ///< (
        RPAREN,   ///< )
        LBRACKET, ///< [
        RBRACKET, ///< ]
        LBRACE,   ///< {
        RBRACE,   ///< }

        // --- OPERATORS ---
        PLUS,    ///< +
        MINUS,   ///< -
        STAR,    ///< *
        SLASH,   ///< /
        PERCENT, ///< %

        // --- ASSIGNMENT / COMPARISON ---
        EQUAL,     ///< =
        COLON,     ///< :
        SEMICOLON, ///< ;
        COMMA,     ///< ,

        // --- SPECIAL SYMBOLS ---
        DOT,        ///< .
        AT,         ///< @
        DOLLAR,     ///< $
        BACKSLASH,  ///< \/
        AMPERSAND,  ///< &
        QUESTION,   ///< ?
        INVERTED_Q, ///< ¿
        PIPE,       ///< |
        UNDERSCORE, ///< _

        COMMENT, ///< Comentario
        NEWLINE, ///< Salto de linea \n

        SYMBOL, ///< Simbolo generico no reconocido
        END_LABEL,
        EndOfFile ///< Fin de archivo (EOF)
    };

    /**
     * @struct Token
     * @brief Representa un token identificado por el lexer.
     */
    struct Token {
        TokenType   type = TokenType::EndOfFile; ///< Tipo de token
        std::string lexeme;                      ///< Texto original del token
        int         line   = 0;                  ///< Linea donde se encuentra
        int         column = 0;                  ///< Columna donde empieza

        /**
         * @brief Constructor de Token
         * @param t Tipo de token
         * @param l Lexema del token
         * @param ln Linea del token
         * @param col Columna del token
         */
        Token(TokenType t, std::string l, int ln, int col)
            : type(t), lexeme(std::move(l)), line(ln), column(col) {}
    };

    /**
     * @class Lexer
     * @brief Convierte código fuente en tokens para el VMProject.
     */
    class Lexer {
    private:
        std::string source;     ///< Código fuente completo
        size_t      pos    = 0; ///< Posición actual en source
        int         line   = 1; ///< Linea actual
        int         column = 1; ///< Columna actual

        /**
         * @brief Devuelve el caracter actual sin avanzar la posición.
         * @return Caracter actual o '\0' si se llegó al final.
         */
        char peek() const;

        /**
         * @brief Devuelve el caracter actual y avanza la posición.
         * Actualiza linea y columna.
         * @return Caracter actual.
         */
        char advance();

        /**
         * @brief Salta espacios en blanco y tabulaciones.
         */
        void skip_whitespace();

        /**
         * @brief Maneja errores del lexer.
         * @param tok Token donde ocurrió el error
         * @param msg Mensaje descriptivo del error
         */
        void error(const Token &tok, const std::string &msg);

        // Constructor privado para peek_token (copia estado)
        Lexer(std::string src, size_t p, int ln, int col)
            : source(std::move(src)), pos(p), line(ln), column(col) {}

    public:
        /**
         * @brief Constructor del lexer
         * @param src Código fuente a tokenizar
         */
        explicit Lexer(std::string src);

        /**
         * @brief Obtiene el siguiente token del código fuente.
         * @return Token siguiente
         */
        Token next_token();

        char peek_token(int offset) const;

        /**
         * @brief Retorna el SIGUIENTE token SIN consumirlo.
         * @return Token que estaría en next_token() sin avanzar pos
         */
        [[nodiscard]] Token peek_token() const;
    };

    std::string token_type_to_string(vm::TokenType type);
} // namespace vm

#endif //LEXER_H
