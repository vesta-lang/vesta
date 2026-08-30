/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file spacing.cpp
 * @brief Cuando va un espacio entre dos tokens: `R33`-`R35`, `R52`,
 * `R61`-`R64`.
 *
 * Aqui NO se adivina nada.  La ambiguedad -- que `a * b` y `i64* p` son el
 * mismo par de tokens -- la resolvio antes @ref annotate_roles, que si lleva el
 * contexto para saberlo; este fichero recibe el papel ya decidido y solo aplica
 * la regla de espaciado que le toca.
 *
 * Dos cosas van ANTES que cualquier regla de estilo, porque no son estilo sino
 * correccion: dos palabras nunca se pegan (`returnx` no es `return x`) y dos
 * signos que se funden en uno mas largo, tampoco.  El peor de esos es `//`, que
 * convierte el resto de la linea en un comentario.
 */

#include "vx/fmt/fmt_internal.h"

#include "vx/token.h"

namespace vx {
namespace fmt {
namespace {

/// @brief Convierte el campo @c kind de una pieza a su enum.
inline TokenKind kind_of(const Piece &p) {
    return static_cast<TokenKind>(p.kind);
}

/**
 * @brief Indica si un token puede TERMINAR un valor.
 *
 * Es la pregunta que decide si el `-` que viene detras es una resta o una
 * negacion: solo se resta de algo que ya es un valor.
 *
 * @param k Categoria del token anterior.
 * @return Cierto si lo que hay detras es un valor completo.
 */
bool ends_value(TokenKind k) {
    switch (k) {
    case TokenKind::IDENTIFIER:
    case TokenKind::INT_LIT:
    case TokenKind::FLOAT_LIT:
    case TokenKind::CHAR_LIT:
    case TokenKind::STRING_LIT:
    case TokenKind::RAW_STRING_LIT:
    case TokenKind::TRUE_KW:
    case TokenKind::FALSE_KW:
    case TokenKind::NULL_KW:
    case TokenKind::RPAREN:
    case TokenKind::RBRACKET:
    case TokenKind::PLUS_PLUS:
    case TokenKind::MINUS_MINUS:
    case TokenKind::KW_THIS:
    case TokenKind::KW_SUPER: return true;
    default: return false;
    }
}

/**
 * @brief Indica si la palabra clave lleva su condicion entre parentesis.
 *
 * Es lo que separa `if (` de `foo(` (`R52` frente a `R33`): el espacio dice de
 * un vistazo que eso no es una llamada.
 *
 * @param k Categoria del token.
 * @return Cierto si es una palabra clave de control con parentesis.
 */
bool keyword_takes_paren(TokenKind k) {
    switch (k) {
    case TokenKind::KW_IF:
    case TokenKind::KW_WHILE:
    case TokenKind::KW_FOR:
    case TokenKind::KW_CATCH:
    case TokenKind::KW_SYNCHRONIZED:
    case TokenKind::KW_MONITOR:
    case TokenKind::KW_MATCH: return true;
    default: return false;
    }
}

/**
 * @brief Indica si el token es un operador binario INEQUIVOCO.
 *
 * Inequivoco quiere decir que no puede ser otra cosa en ningun contexto.  Los
 * que si pueden -- `*`, `&`, `<`, `>` -- se quedan fuera a proposito.
 *
 * @param k Categoria del token.
 * @return Cierto si siempre es binario.
 */
bool always_binary(TokenKind k) {
    switch (k) {
    case TokenKind::SLASH:
    case TokenKind::PERCENT:
    case TokenKind::EQ:
    case TokenKind::NEQ:
    case TokenKind::LE:
    case TokenKind::GE:
    case TokenKind::AND_AND:
    case TokenKind::OR_OR:
    case TokenKind::ASSIGN:
    case TokenKind::PLUS_ASSIGN:
    case TokenKind::MINUS_ASSIGN:
    case TokenKind::STAR_ASSIGN:
    case TokenKind::SLASH_ASSIGN:
    case TokenKind::PERCENT_ASSIGN:
    case TokenKind::AMP_ASSIGN:
    case TokenKind::PIPE_ASSIGN:
    case TokenKind::CARET_ASSIGN:
    case TokenKind::SHL_ASSIGN:
    case TokenKind::SHR_ASSIGN:
    case TokenKind::PIPE:
    case TokenKind::CARET:
    case TokenKind::FAT_ARROW:
    case TokenKind::ARROW: return true;
    default: return false;
    }
}

/**
 * @brief Indica si el token es un prefijo que va PEGADO a lo que sigue (`R62`).
 * @param k Categoria del token.
 * @return Cierto si nunca lleva espacio detras.
 */
bool is_prefix(TokenKind k) {
    return k == TokenKind::BANG || k == TokenKind::TILDE ||
           k == TokenKind::PLUS_PLUS || k == TokenKind::MINUS_MINUS ||
           k == TokenKind::AT;
}

/**
 * @brief Indica si el texto empieza o acaba en caracter de palabra.
 * @param c Caracter.
 * @return Cierto si es letra, digito o subrayado.
 */
bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/// @brief Cierto si el caracter es un signo de operador.
bool is_sign(char c) {
    switch (c) {
    case '+':
    case '-':
    case '*':
    case '/':
    case '%':
    case '=':
    case '<':
    case '>':
    case '&':
    case '|':
    case '^':
    case '!':
    case '~':
    case ':': return true;
    default: return false;
    }
}

/**
 * @brief Cierto si los dos caracteres, pegados, formarian OTRO token.
 *
 * La pregunta que de verdad importa antes de quitar un espacio.  Se mira el
 * PAR y no cada signo por su cuenta: `--`, `->`, `==` o el peor de todos,
 * `//`, cambian el programa; `**` no es ningun token de Vesta y puede ir
 * pegado, que es lo que `i64**` necesita.
 *
 * @param a Ultimo caracter del token de la izquierda.
 * @param b Primer caracter del de la derecha.
 */
bool forms_other_token(char a, char b) {
    switch (a) {
    case '+': return b == '+' || b == '=';
    case '-': return b == '-' || b == '=' || b == '>';
    case '*': return b == '=';
    case '/': return b == '/' || b == '*' || b == '=';
    case '%': return b == '=';
    case '=': return b == '=' || b == '>';
    case '<': return b == '=' || b == '<';
    case '>': return b == '=' || b == '>';
    case '&': return b == '&' || b == '=';
    case '|': return b == '|' || b == '=';
    case '^': return b == '=';
    case '!': return b == '=' || b == '!';
    case '.': return b == '.';
    case ':': return b == ':';
    default: return false;
    }
}

} // namespace

Spacing space_between(const Piece *before, const Piece &prev, const Piece &cur,
                      Role prev_role, Role cur_role) {
    const TokenKind a = kind_of(prev);
    const TokenKind b = kind_of(cur);

    if (prev.text.empty() || cur.text.empty()) return Spacing::Keep;
    const char left = prev.text.back();
    const char right = cur.text.front();

    /* DOS SALVAGUARDAS, y van LO PRIMERO para que ninguna regla de estilo pueda
     * saltarselas.  Las dos dicen lo mismo: hay pares de tokens que, pegados,
     * dejan de ser esos dos tokens.
     *
     * 1. Dos PALABRAS.  `return x` pegado seria `returnx`, un identificador
     *    distinto.
     * 2. Dos SIGNOS que se funden en uno mas largo.  `a - -b` pegado da `a--b`
     *    -- un decremento donde habia una resta de un negativo --, y el peor de
     *    todos: dos barras seguidas convierten el resto de la linea en un
     *    comentario y se lleva por delante media funcion.
     *
     * Aqui el espacio no es estilo: es el programa.
     *
     * La segunda mira el PAR, no cada signo por su cuenta.  Bastaba con que los
     * dos fueran signos, y eso separaba tambien pares inofensivos: `**` no es
     * ningun token de Vesta, asi que `i64**` salia como `i64* *`.  La pregunta
     * correcta no es "son signos" sino "juntos serian OTRA cosa". */
    if (is_word_char(left) && is_word_char(right)) return Spacing::Space;
    if (forms_other_token(left, right)) return Spacing::Space;

    // --- Lo que NUNCA lleva espacio delante (`R34`, `R63`, `R64`). ---
    switch (b) {
    case TokenKind::COMMA:
    case TokenKind::SEMICOLON:
    case TokenKind::RPAREN:
    case TokenKind::RBRACKET:
    case TokenKind::DOT:
    case TokenKind::LBRACKET: return Spacing::None;
    default: break;
    }

    /* EL PAPEL MANDA.  Lo decidio el pase que si tiene contexto
     * (@ref annotate_roles), asi que aqui no se vuelve a adivinar.
     *
     * Cada papel dice algo distinto de CADA LADO, y eso importa: el `*` de un
     * puntero va pegado al TIPO pero separado del NOMBRE -- `i64* p` --, y el
     * `>` que cierra un generico va pegado al tipo pero separado de lo que
     * sigue -- `Caja<i64> c` --.  Tratarlos como "pegado y ya" daba `i64*p`. */
    // Pegado por delante: `i64*`, `Caja<`, `i64>`, `case X:`.
    if (cur_role == Role::TightBoth || cur_role == Role::TightLeft)
        return Spacing::None;
    // Pegado por detras: `<i64`.
    if (prev_role == Role::TightBoth) return Spacing::None;
    if (prev_role == Role::TightLeft) {
        /* Separado detras -- `i64* p`, `Caja<i64> c` -- salvo cuando lo que
         * sigue nunca lleva espacio delante: `hacer<i64>()` sigue siendo una
         * llamada, y `Caja<i64>,` no separa la coma. */
        if (b == TokenKind::LPAREN) return Spacing::None;
        return Spacing::Space;
    }
    if (cur_role == Role::TightRight) {
        /* Pegado a lo que sigue, pero separado de lo de antes cuando eso era un
         * valor o una palabra: `a - -b`, `return *p`. */
        /* Tambien detras de un OPERADOR: `p = &x` necesita su espacio, y sin
         * mirarlo salia `p =&x`.  Antes lo tapaba una salvaguarda que separaba
         * cualquier par de signos; al hacerla precisa -- para que `i64**` se
         * pegara -- este caso quedo al aire. */
        /* Se separa de lo de antes cuando eso ya pedia un espacio detras: un
         * valor, una palabra, un operador... o un SEPARADOR.  La coma es el
         * caso que se colaba: `f(a, &b)` salia `f(a,&b)`, porque el papel del
         * `&` decidia aqui antes de que `R35` llegara a decir lo suyo. */
        const bool needs = ends_value(a) || is_word_char(left) ||
                           is_sign(left) || a == TokenKind::COMMA ||
                           a == TokenKind::SEMICOLON;
        return needs ? Spacing::Space : Spacing::None;
    }
    if (prev_role == Role::TightRight) return Spacing::None; // `*p`, `&x`
    if (cur_role == Role::Binary || prev_role == Role::Binary)
        return Spacing::Space;

    // --- Lo que NUNCA lleva espacio detras (`R34`, `R62`, `R64`). ---
    if (a == TokenKind::LPAREN || a == TokenKind::LBRACKET ||
        a == TokenKind::DOT || is_prefix(a))
        return Spacing::None;

    // `R35`: una coma siempre lleva un espacio detras.
    if (a == TokenKind::COMMA) return Spacing::Space;

    /* `R54`: y un punto y coma tambien, cuando le sigue algo en la misma
     * linea.  El unico sitio donde eso pasa es la cabecera de un `for`
     * clasico: `for (i = 0; i < n; i = i + 1)`. */
    if (a == TokenKind::SEMICOLON) return Spacing::Space;

    /* `R44`: un cuerpo vacio se escribe `{ }`.
     *
     * Va antes que `R34`, que pega los cierres a lo anterior: sin esto salia
     * `{}`, que se confunde con una lista de inicializacion vacia. */
    if (a == TokenKind::LBRACE && b == TokenKind::RBRACE) {
        // Salvo una lista de inicializacion vacia, que no es un cuerpo: ahi
        // `= {}` es la forma corriente de decir "ninguno".
        const bool lista =
            before != nullptr && kind_of(*before) == TokenKind::ASSIGN;
        return lista ? Spacing::None : Spacing::Space;
    }

    // `R4`: la llave de apertura va al final de la linea, con un espacio.
    if (b == TokenKind::LBRACE) return Spacing::Space;

    /* `R33` frente a `R52`: `foo(` va pegado y `if (` separado.  El espacio es
     * lo que distingue de un vistazo una llamada de una estructura de control,
     * y por eso no es un capricho de estilo. */
    if (b == TokenKind::LPAREN) {
        if (keyword_takes_paren(a)) return Spacing::Space;
        if (ends_value(a)) return Spacing::None; // llamada o agrupacion
        return Spacing::Keep;
    }

    // `R61`: un espacio a cada lado de los binarios que no pueden ser otra
    // cosa.
    if (always_binary(a) || always_binary(b)) return Spacing::Space;

    /* `+` y `-`: como no son ambiguos en su forma -- solo en si son signo u
     * operacion --, se deciden aqui con la misma regla del valor. */
    if (b == TokenKind::PLUS || b == TokenKind::MINUS) {
        /* Espacio SIEMPRE, sea resta (`x - y`) o signo (`return -1`).
         *
         * Lo que de verdad va pegado por delante -- detras de un `(`, un `[`,
         * una `.` o un prefijo como `!` -- ya se resolvio mas arriba, asi que
         * a esta rama solo llega lo que necesita separarse.
         *
         * Aqui habia una condicion que solo daba espacio si lo de antes
         * acababa en caracter de PALABRA.  Dejaba dos casos pegados: detras de
         * otro signo salia `a +-1` -- y `+-` juntos se leen como un operador
         * que no existe -- y, comprobado formateando el corpus, tampoco
         * separaba `return -1`, que salia `return-1` en 21 ejemplos pese a que
         * el comentario de esta misma rama decia estar cubriendo ese caso. */
        return Spacing::Space;
    }
    if (a == TokenKind::PLUS || a == TokenKind::MINUS) {
        /* Para saber si lleva espacio DETRAS hay que saber que era, y eso lo
         * dice el token de antes: `x - y` resta, `(-y)` niega.  Por eso hace
         * falta mirar dos atras y no una. */
        const bool binary = before != nullptr && ends_value(kind_of(*before));
        return binary ? Spacing::Space : Spacing::None;
    }

    /* Lo que llega aqui sin papel asignado es lo que el anotador no supo
     * decidir, y tiene que ser raro: se conserva lo que habia. */
    return Spacing::Keep;
}

} // namespace fmt
} // namespace vx
