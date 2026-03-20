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

#include <utility>

#include "lexer/lexer.h"

namespace vm {
    Lexer::Lexer(std::string src) : source(std::move(src)), pos(0), line(1), column(1) {}


    char Lexer::peek() const {
        return pos < source.size() ? source[pos] : '\0';
    }

    char Lexer::advance() {
        char c = peek();
        if (c == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
        ++pos;
        return c;
    }

    void Lexer::skip_whitespace() {
        while (true) {
            if (isspace(peek())) {
                advance();
                continue;
            }

            // ---------------- COMENTARIOS // ----------------
            if (peek() == '/' && peek_token(1) == '/') {  // peek_token(1) mira 1 char adelante
                while (peek() != '\n' && peek() != '\0') {
                    advance();
                }
                continue;
            }

            // ---------------- COMENTARIOS /* */ ---------------- (opcional)
            if (peek() == '/' && peek_token(1) == '*') {
                advance(); advance();  // Saltar /*
                while (!(peek() == '*' && peek_token(1) == '/') && peek() != '\0') {
                    advance();
                }
                if (peek() == '*' && peek_token(1) == '/') {
                    advance(); advance();  // Saltar */
                }
                continue;
            }

            break;  // No más whitespace/comentarios
        }
    }

    void Lexer::error(const Token &tok, const std::string &msg) {
        std::cerr << "Lexer Error: " << msg
                << " en linea " << tok.line
                << ", columna " << tok.column
                << " (token: '" << tok.lexeme << "')\n";
    }

    Token Lexer::next_token() {
        skip_whitespace();

        if (pos >= source.size())
            return {TokenType::EndOfFile, "", line, column};

        int  start_column = column;
        char c            = advance();

        // ---------------- CHAR LITERALS ----------------
        if (c == '\'') {
            std::string lexeme;
            int         start_line = line;
            int         start_col  = start_column;

            if (peek() == '\0') {
                error(Token(TokenType::CHAR, lexeme, start_line, start_col),
                      "char literal vacio o incompleto");
                return {TokenType::CHAR, lexeme, start_line, start_col};
            }

            if (peek() == '\\') {
                // escape sequence
                lexeme += advance(); // '\'
                if (peek() != '\0')
                    lexeme += advance(); // 'n', 't', etc
                else {
                    error(Token(TokenType::CHAR, lexeme, start_line, start_col),
                          "escape sequence incompleta en char literal");
                    return {TokenType::CHAR, lexeme, start_line, start_col};
                }
            } else {
                lexeme += advance(); // caracter normal
            }

            if (peek() == '\'') {
                advance(); // cerrar comilla
            } else {
                error(Token(TokenType::CHAR, lexeme, start_line, start_col),
                      "char literal no cerrado");
            }

            return {TokenType::CHAR, lexeme, start_line, start_col};
        }

        // ---------------- STRING LITERALS ----------------
        if (c == '"') {
            std::string lexeme;
            int         start_line = line;
            int         start_col  = start_column;

            while (peek() != '"' && peek() != '\0') {
                lexeme += advance();
            }

            if (peek() == '"') {
                advance(); // cerrar comillas
            } else {
                error(Token(TokenType::STRING, lexeme, start_line, start_col),
                      "string literal no cerrado");
            }

            return {TokenType::STRING, lexeme, start_line, start_col};
        }

        if (isalpha(c) || c == '_') {
            std::string lexeme(1, c);

            while (isalnum(peek()) || peek() == '_')
                lexeme += advance();

            /*
            if (
                lexeme == "rax" || lexeme == "rbx" || lexeme == "rcx" || lexeme == "rdx" ||
                lexeme == "eax" || lexeme == "ebx" || lexeme == "ecx" || lexeme == "edx" ||
                lexeme == "sp" || lexeme == "bp" || lexeme == "ip"
            ) {
                return {TokenType::REGISTER, lexeme, line, start_column};
            }*/

            // registros tipo r0 r1 r2 r15, ...
            if (is_register(lexeme)) {
                return {TokenType::REGISTER, lexeme, line, start_column};
            }

            if (is_data_directive(lexeme)) {
                return {TokenType::DATA_DIRECTIVE, lexeme, line, start_column};
            }

            return {TokenType::IDENTIFIER, lexeme, line, start_column};
        }

        if (isdigit(c)) {
            std::string lexeme(1, c);

            if (c == '0') {
                char p = peek();

                if (p == 'x' || p == 'X') {
                    lexeme += advance();
                    while (isxdigit(peek()))
                        lexeme += advance();
                    return {TokenType::NUMBER_HEX, lexeme, line, start_column};
                }

                if (p == 'b' || p == 'B') {
                    lexeme += advance();
                    while (peek() == '0' || peek() == '1')
                        lexeme += advance();
                    return {TokenType::NUMBER_BIN, lexeme, line, start_column};
                }

                if (p == 'o' || p == 'O') {
                    lexeme += advance();
                    while (peek() >= '0' && peek() <= '7')
                        lexeme += advance();
                    return {TokenType::NUMBER_OCT, lexeme, line, start_column};
                }
            }

            bool is_float = false;

            while (isdigit(peek()))
                lexeme += advance();

            if (peek() == '.') {
                is_float = true;
                lexeme += advance();

                while (isdigit(peek()))
                    lexeme += advance();
            }

            if (is_float)
                return {TokenType::NUMBER_FLOAT, lexeme, line, start_column};

            return {TokenType::NUMBER_DEC, lexeme, line, start_column};
        }

        if (c == ';') {
            std::string comment;

            while (peek() != '\n' && peek() != '\0')
                comment += advance();

            return {TokenType::COMMENT, comment, line, start_column};
        }

        switch (c) {
            case '=': return {TokenType::EQUAL, "=", line, start_column};
            case ':': return {TokenType::COLON, ":", line, start_column};
            case ',': return {TokenType::COMMA, ",", line, start_column};

            case '(': return {TokenType::LPAREN, "(", line, start_column};
            case ')': return {TokenType::RPAREN, ")", line, start_column};

            case '[': return {TokenType::LBRACKET, "[", line, start_column};
            case ']': return {TokenType::RBRACKET, "]", line, start_column};

            case '{': return {TokenType::LBRACE, "{", line, start_column};
            case '}': return {TokenType::RBRACE, "}", line, start_column};

            case '+': return {TokenType::PLUS, "+", line, start_column};
            case '-': return {TokenType::MINUS, "-", line, start_column};
            case '*': return {TokenType::STAR, "*", line, start_column};
            case '/': return {TokenType::SLASH, "/", line, start_column};
            case '%': return {TokenType::PERCENT, "%", line, start_column};

            case '.': return {TokenType::DOT, ".", line, start_column};
            case '@': return {TokenType::AT, "@", line, start_column};
            case '$': return {TokenType::DOLLAR, "$", line, start_column};
            case '\\': return {TokenType::BACKSLASH, "\\", line, start_column};
            case '&': return {TokenType::AMPERSAND, "&", line, start_column};
            case '?': return {TokenType::QUESTION, "?", line, start_column};
            case '|': return {TokenType::PIPE, "|", line, start_column};
            case '_': return {TokenType::UNDERSCORE, "_", line, start_column};

            default:
                return {TokenType::SYMBOL, std::string(1, c), line, start_column};
        }
    }

    char Lexer::peek_token(int offset) const {
        size_t idx = pos + offset;
        return (idx < source.size()) ? source[idx] : '\0';
    }

    Token Lexer::peek_token() const {
        // Guardar estado actual
        const size_t save_pos = pos;
        int save_line = line;
        int save_column = column;

        // Crear lexer temporal con mismo estado
        Lexer temp(source, save_pos, save_line, save_column);

        // Obtener token sin modificar estado real
        Token peeked = temp.next_token();

        return peeked;
    }

    static const char *token_names[] = {
        "NUMBER_HEX", "NUMBER_BIN", "NUMBER_OCT", "NUMBER_FLOAT", "NUMBER_DEC",

        "IDENTIFIER", "DATA_DIRECTIVE", "REGISTER",

        "CHAR", "STRING",

        "LPAREN", "RPAREN", "LBRACKET", "RBRACKET", "LBRACE", "RBRACE",

        "PLUS", "MINUS", "STAR", "SLASH", "PERCENT",

        "EQUAL", "COLON", "SEMICOLON", "COMMA",

        "DOT", "AT", "DOLLAR", "BACKSLASH", "AMPERSAND", "QUESTION", "INVERTED_Q", "PIPE", "UNDERSCORE",

        "COMMENT", "NEWLINE",

        "SYMBOL", "EndOfFile"
    };

    std::string token_type_to_string(vm::TokenType type) {
        auto idx = static_cast<int>(type);
        if (idx < 0 || idx >= std::size(token_names)) return "UNKNOWN";
        return token_names[idx];
    }
} // namespace vm
