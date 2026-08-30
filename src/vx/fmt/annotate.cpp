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
 * @file annotate.cpp
 * @brief Que PAPEL juega cada token ambiguo.
 *
 * En Vesta -- como en C, Java o C# -- hay simbolos que significan cosas
 * distintas segun donde esten, y el espaciado correcto depende de cual sea:
 *
 *     a * b     producto, con espacios      i64* p    puntero, pegado
 *     a & b     bits, con espacios          &x        direccion, pegado
 *     a < b     comparacion, con espacios   Caja<i64> tipo, pegado
 *     c ? a : b ternario, con espacios      Animal?   anulable, pegado
 *
 * Mirando solo el token de al lado no se puede saber: `IDENT * IDENT` es lo
 * mismo en `foo(a * b)` que en `f(Punto* p)`.  Hace falta CONTEXTO.
 *
 * Y no hace falta el arbol.  Este pase lleva el estado justo -- si se esta al
 * principio de una sentencia, si lo que se lee es una declaracion, si un `<`
 * cierra como lista de tipos -- y con eso resuelve el papel de cada simbolo en
 * una sola pasada.  Es el mismo reparto que hace clang-format: un pase anota,
 * otro formatea.  Asi el formateador sigue funcionando con codigo a medio
 * escribir, que era la razon de no depender del parser.
 *
 * Lo que de verdad no se pueda decidir se queda en @c Role::Unknown, y quien
 * formatea conserva lo que habia.  Pero eso tiene que ser la excepcion rara, no
 * la respuesta por defecto.
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
 * @brief Indica si el token puede TERMINAR un valor.
 *
 * Es la pregunta que separa un operador binario de uno unario: solo se
 * multiplica algo que ya es un valor.
 *
 * @param k Categoria del token.
 * @return Cierto si lo de detras es un valor completo.
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
 * @brief Indica si el token nombra un TIPO del lenguaje.
 *
 * Son todas las palabras clave de tipo, y en Vesta esas incluyen las
 * COLECCIONES: `ArrayList`, `HashMap`, `Queue` y compania no son
 * identificadores de una libreria, son tipos primitivos del lenguaje.
 * Tratarlos como nombres corrientes hacia que `HashMap<string, i64>` no se
 * reconociera como generico y saliera formateado `HashMap < string, i64>`.
 *
 * Se usa el RANGO del enum, que va seguido desde `void` hasta `borrow_mut`, en
 * vez de enumerarlas: una lista escrita a mano se queda corta en cuanto el
 * lenguaje gana un tipo, y se queda corta EN SILENCIO.
 *
 * @param k Categoria del token.
 * @return Cierto si nombra un tipo.
 */
bool is_type_keyword(TokenKind k) {
    return k >= TokenKind::KW_VOID && k <= TokenKind::KW_BORROW_MUT;
}

/**
 * @brief Indica si el token puede aparecer DENTRO de un tipo.
 *
 * Es lo que decide si un `<` abre una lista de argumentos de tipo: se mira
 * hacia adelante y, si hasta el `>` que cierra solo hay cosas que caben en un
 * tipo, era un generico.  En cuanto aparece algo que no cabe -- un literal, un
 * `+`, un `;` -- era una comparacion.
 *
 * @param k Categoria del token.
 * @return Cierto si cabe dentro de un tipo.
 */
bool fits_in_type(TokenKind k) {
    if (is_type_keyword(k)) return true;
    switch (k) {
    case TokenKind::IDENTIFIER:
    case TokenKind::COMMA:
    case TokenKind::STAR:
    case TokenKind::AMP:
    case TokenKind::LBRACKET:
    case TokenKind::RBRACKET:
    case TokenKind::QUESTION:
    case TokenKind::DOT:
    case TokenKind::LT:
    case TokenKind::GT:
    case TokenKind::SHR: // `>>` cerrando dos genericos anidados
    case TokenKind::INT_LIT: return true; // `u8[16]`
    default: return false;
    }
}

/**
 * @brief Indica si el token abre una sentencia nueva.
 * @param k Categoria del token.
 * @return Cierto si lo que sigue empieza de cero.
 */
bool starts_statement(TokenKind k) {
    return k == TokenKind::SEMICOLON || k == TokenKind::LBRACE ||
           k == TokenKind::RBRACE || k == TokenKind::COLON;
}

/**
 * @brief Indica si el token es un modificador que precede a un tipo.
 * @param k Categoria del token.
 * @return Cierto si es `public`, `const`, `static` y compania.
 */
bool is_modifier(TokenKind k) {
    switch (k) {
    case TokenKind::KW_PUBLIC:
    case TokenKind::KW_PRIVATE:
    case TokenKind::KW_PROTECTED:
    case TokenKind::KW_STATIC:
    case TokenKind::KW_FINAL:
    case TokenKind::KW_CONST: return true;
    default: return false;
    }
}

/**
 * @brief Busca el `>` que cierra un `<` que empieza en @p open.
 *
 * @param pieces Todas las piezas.
 * @param open   Indice del `<`.
 * @param close  [out] indice del `>` que cierra, si lo hay.
 * @return Cierto si el `<` abre una lista de argumentos de tipo.
 */
bool closes_as_type_args(const std::vector<Piece> &pieces, size_t open,
                         size_t &close) {
    /* Antes de un `<` de generico va SIEMPRE un nombre de tipo.  Tras un
     * literal o un `)` solo puede ser una comparacion. */
    if (open == 0) return false;
    const TokenKind before = kind_of(pieces[open - 1]);
    if (before != TokenKind::IDENTIFIER && !is_type_keyword(before))
        return false;

    int depth = 0;
    // Un tope corto: una lista de tipos larguisima no existe, y sin el una
    // comparacion haria recorrer el fichero entero por cada `<`.
    const size_t limit = open + 64 < pieces.size() ? open + 64 : pieces.size();
    for (size_t i = open; i < limit; ++i) {
        const TokenKind k = kind_of(pieces[i]);
        if (k == TokenKind::LT) {
            ++depth;
            continue;
        }
        if (k == TokenKind::GT) {
            if (--depth == 0) {
                close = i;
                return true;
            }
            continue;
        }
        if (k == TokenKind::SHR) { // `>>` cierra dos de golpe
            depth -= 2;
            if (depth <= 0) {
                close = i;
                return true;
            }
            continue;
        }
        if (i > open && !fits_in_type(k)) return false;
    }
    return false;
}

} // namespace

std::vector<Role> annotate_roles(const std::vector<Piece> &pieces) {
    std::vector<Role> roles(pieces.size(), Role::Plain);

    /* `decl_until` marca hasta donde llega el TIPO de una declaracion que se
     * esta leyendo.  Mientras se esta dentro, un `*` es un puntero y un `<` una
     * lista de argumentos; fuera, son producto y comparacion. */
    size_t decl_until = 0;
    /* Profundidad de la lista de PARAMETROS en la que estamos, o -1.
     *
     * Dentro de ella cada elemento es una declaracion, asi que un `*` es un
     * puntero y va pegado al tipo (`R27`).  En una LLAMADA no: ahi `f(a * b)`
     * es una multiplicacion, y por eso no vale con mirar cualquier `(`.  Se
     * distingue por como se llego: el `(` de una declaracion viene justo
     * detras del nombre que `decl_until` acaba de marcar. */
    size_t decl_name = 0; // indice del nombre de la ultima declaracion vista
    int param_paren = -1;
    int depth_paren = 0;
    bool stmt_start = true;

    /* Contexto para los DOS PUNTOS, que en Vesta son seis cosas distintas:
     *
     *     c ? a : b        ternario         -> espacios a los dos lados
     *     class A : B      herencia         -> espacios
     *     for (T x : xs)   recorrido        -> espacios
     *     case Foo:        rama             -> pegado delante
     *     salir:           etiqueta         -> pegado delante
     *     where T: Comp    restriccion      -> pegado delante
     *
     * No se distinguen por lo que tienen al lado, sino por DONDE estan.  Basta
     * con recordar unas pocas cosas mientras se recorre. */
    int ternary_open = 0;     // `?` de ternario esperando su `:`
    int annot_depth = 0;      // profundidad de los parentesis de `@Nombre(`
    bool label_seen = false;  // ya salio el `:` de la etiqueta del argumento
    int paren_depth = 0;      // parentesis abiertos
    bool after_case = false;  // se vio un `case` sin cerrar
    bool after_where = false; // se vio un `where` sin cerrar
    bool in_for = false;      // dentro de los parentesis de un `for`
    bool stmt_start_prev = false; // el token anterior abria sentencia
    int for_paren = -1;           // profundidad a la que abrio ese `for`

    for (size_t i = 0; i < pieces.size(); ++i) {
        const TokenKind k = kind_of(pieces[i]);

        /* Al principio de una sentencia, mirar si lo que empieza es una
         * DECLARACION: `[modificadores] TIPO [*|[]|<>] NOMBRE` seguido de `=`,
         * `;`, `,`, `)` o `(`.  Es el unico sitio donde hace falta mirar
         * adelante, y basta con unos pocos tokens. */
        if (stmt_start && i >= decl_until) {
            size_t j = i;
            while (j < pieces.size() && is_modifier(kind_of(pieces[j])))
                ++j;
            const bool named_type =
                j < pieces.size() &&
                (is_type_keyword(kind_of(pieces[j])) ||
                 kind_of(pieces[j]) == TokenKind::IDENTIFIER);
            if (named_type) {
                size_t t = j + 1;
                // Saltar lo que decora un tipo: punteros, arrays, genericos.
                while (t < pieces.size()) {
                    const TokenKind tk = kind_of(pieces[t]);
                    if (tk == TokenKind::STAR || tk == TokenKind::AMP ||
                        tk == TokenKind::LBRACKET ||
                        tk == TokenKind::RBRACKET ||
                        tk == TokenKind::QUESTION || tk == TokenKind::INT_LIT) {
                        ++t;
                        continue;
                    }
                    if (tk == TokenKind::LT) {
                        size_t close = 0;
                        if (!closes_as_type_args(pieces, t, close)) break;
                        t = close + 1;
                        continue;
                    }
                    break;
                }
                // Tras el tipo tiene que venir un nombre, y tras el nombre algo
                // que solo aparece en una declaracion.
                const bool has_name =
                    t < pieces.size() &&
                    kind_of(pieces[t]) == TokenKind::IDENTIFIER && t > j;
                if (has_name && t + 1 < pieces.size()) {
                    switch (kind_of(pieces[t + 1])) {
                    case TokenKind::ASSIGN:
                    case TokenKind::SEMICOLON:
                    case TokenKind::COMMA:
                    case TokenKind::RPAREN:
                    case TokenKind::LPAREN:
                        decl_until = t;
                        // `decl_until` se pone a cero en cuanto se alcanza, y
                        // para entonces ya se paso el `(`.  El nombre se
                        // guarda aparte para poder reconocerlo alli.
                        decl_name = t;
                        break;
                    default: break;
                    }
                }
            }
        }

        switch (k) {
        case TokenKind::STAR:
            if (i < decl_until) {
                roles[i] = Role::TightLeft; // `i64* p`
            } else if (i > 0 && ends_value(kind_of(pieces[i - 1]))) {
                roles[i] = Role::Binary; // `a * b`
            } else {
                roles[i] = Role::TightRight; // `*p`
            }
            break;
        case TokenKind::AMP:
            if (i < decl_until) {
                roles[i] = Role::TightLeft;
            } else if (i > 0 && ends_value(kind_of(pieces[i - 1]))) {
                roles[i] = Role::Binary; // `a & b`
            } else {
                roles[i] = Role::TightRight; // `&x`
            }
            break;
        case TokenKind::LT: {
            size_t close = 0;
            if (closes_as_type_args(pieces, i, close)) {
                roles[i] = Role::TightBoth;
                roles[close] = Role::TightLeft;
                // Lo de dentro es un tipo: sus `*` van pegados.
                if (close > decl_until) decl_until = close;
            } else if (roles[i] == Role::Plain) {
                roles[i] = Role::Binary; // `a < b`
            }
            break;
        }
        case TokenKind::GT:
            // Si no lo marco ya el `<` que abria, es una comparacion.
            if (roles[i] == Role::Plain) roles[i] = Role::Binary;
            break;
        case TokenKind::QUESTION:
            /* `Animal?` va pegado al tipo; `c ? a : b` lleva espacios.  Dentro
             * de una declaracion es anulable; fuera, ternario -- y entonces hay
             * que recordar que su `:` viene despues. */
            if (i < decl_until) {
                roles[i] = Role::TightLeft;
            } else {
                roles[i] = Role::Binary;
                ++ternary_open;
            }
            break;
        case TokenKind::DOTDOTDOT:
            /* `R37`: el variadico va pegado al TIPO y separado del nombre,
             * igual que el `*` de un puntero: `i64... xs`.  Es parte del
             * tipo, no un operador entre dos cosas. */
            roles[i] = Role::TightLeft;
            break;
        case TokenKind::CARET:
            /* Dentro de una anotacion, `^` es un EXPONENTE y no un xor:
             * `O(n^k)` es una formula, y separarla la convierte en una
             * expresion que no lo es.  Fuera, el espaciado normal. */
            if (annot_depth > 0) roles[i] = Role::TightBoth;
            break;
        case TokenKind::COLON:
            if (annot_depth > 0) {
                /* Dentro de una anotacion, `nombre: valor`.
                 *
                 * El PRIMER `:` de cada argumento es el de su etiqueta y va
                 * pegado al nombre, como en un mapa.  Los siguientes son parte
                 * del VALOR y van pegados por los dos lados, porque ahi
                 * `clave:valor` es una sola cosa:
                 *
                 *     @stack(partial: 0, total: 32, when: arch:arm64)
                 *                                         ^^^^ un solo atomo
                 *
                 * Se distinguen por el orden dentro del argumento, que es lo
                 * unico que hace falta: la cuenta se reinicia en cada coma. */
                roles[i] = label_seen ? Role::TightBoth : Role::TightLeft;
                label_seen = true;
            } else if (ternary_open > 0) {
                roles[i] = Role::Binary; // cierra un ternario
                --ternary_open;
            } else if (after_case || after_where) {
                roles[i] = Role::TightLeft; // `case Foo:`, `where T: Comp`
                after_case = false;
                after_where = false;
            } else if (in_for) {
                roles[i] = Role::Binary; // `for (T x : xs)`
            } else if (stmt_start_prev) {
                /* Al principio de una sentencia y sin nada mas: es una
                 * etiqueta de `goto`.  `salir:` va pegado. */
                roles[i] = Role::TightLeft;
            } else {
                roles[i] = Role::Binary; // herencia: `class A : B`
            }
            break;
        default: break;
        }

        // --- Contabilidad del contexto para la proxima vuelta. ---
        if (k == TokenKind::LPAREN) {
            ++depth_paren;
            // El `(` que sigue al nombre de una declaracion abre parametros.
            if (param_paren < 0 && i > 0 && decl_name == i - 1 &&
                kind_of(pieces[i - 1]) == TokenKind::IDENTIFIER)
                param_paren = depth_paren;
            ++paren_depth;
            if (i > 0 && kind_of(pieces[i - 1]) == TokenKind::KW_FOR) {
                in_for = true;
                for_paren = paren_depth;
            }
            /* `@Nombre(`: los parentesis de una anotacion.  Se anota su
             * profundidad para saber cuando se cierra, porque dentro puede
             * haber otros parentesis que no son suyos (`O(n)`). */
            if (i >= 2 && kind_of(pieces[i - 1]) == TokenKind::IDENTIFIER &&
                kind_of(pieces[i - 2]) == TokenKind::AT) {
                annot_depth = paren_depth;
                label_seen = false;
            }
        } else if (k == TokenKind::RPAREN) {
            if (param_paren == depth_paren) param_paren = -1;
            if (depth_paren > 0) --depth_paren;
            if (annot_depth > 0 && paren_depth == annot_depth) {
                annot_depth = 0;
                label_seen = false;
            }
            if (in_for && paren_depth == for_paren) {
                in_for = false;
                for_paren = -1;
            }
            if (paren_depth > 0) --paren_depth;
        }
        // Cada argumento de la anotacion tiene su propia etiqueta.
        if (k == TokenKind::COMMA && annot_depth > 0) label_seen = false;
        if (k == TokenKind::KW_CASE) after_case = true;
        if (k == TokenKind::IDENTIFIER && pieces[i].text == "where")
            after_where = true;
        if (k == TokenKind::SEMICOLON || k == TokenKind::LBRACE) {
            // Una sentencia nueva no hereda ternarios ni restricciones a
            // medias.
            ternary_open = 0;
            after_case = false;
            after_where = false;
        }

        // Una etiqueta es un NOMBRE al principio de sentencia, y nada mas.
        stmt_start_prev = stmt_start && k == TokenKind::IDENTIFIER;
        stmt_start = starts_statement(k) ||
                     (param_paren >= 0 &&
                      (k == TokenKind::LPAREN || k == TokenKind::COMMA));
        if (i >= decl_until) decl_until = 0;
    }
    return roles;
}

} // namespace fmt
} // namespace vx
