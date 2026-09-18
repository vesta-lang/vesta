/**
 * @file rewrite.cpp
 * @brief Las reglas que cambian TOKENS, no solo espacio (`R29`, `R42`, `R74`).
 *
 * Todo lo demas del formateador mueve blancos.  Estas tres no: funden dos
 * tokens en uno, quitan un par vacio o intercambian dos palabras.  Ninguna
 * cambia el programa -- se comprobo ejecutando las dos formas y comparando el
 * resultado --, pero `P2` no puede verlo por si mismo, porque compara la lista
 * de tokens y esa lista cambia.
 *
 * Por eso cada una DECLARA lo que hace: deja un @c Rewrite anclado al token
 * del original donde ocurre, y la comprobacion de `fmt.cpp` exige que la
 * diferencia entre el antes y el despues sea exactamente esa.  Una diferencia
 * que nadie declaro sigue siendo un fallo.  Asi se abren solo las puertas
 * medidas una a una, y la red se queda igual de fina para todo lo demas.
 */

#include "vx/fmt/fmt_internal.h"

#include "vx/fmt/width.h" // display_width, para medir si el cuerpo cabe
#include "vx/token.h"

namespace vx {
namespace fmt {
namespace {

/**
 * @brief Orden canonico de un modificador (`R42`): acceso, `static`, `final`.
 *
 * El acceso primero porque es lo que se busca al leer una clase por encima;
 * `static` despues, que dice si hace falta una instancia; y `final` al lado
 * del tipo, que es a lo que se refiere.
 *
 * @param k Categoria del token.
 * @return Su puesto, o cero si no es un modificador.
 */
int modifier_rank(TokenKind k) {
    switch (k) {
    case TokenKind::KW_PUBLIC:
    case TokenKind::KW_PRIVATE:
    case TokenKind::KW_PROTECTED: return 1;
    case TokenKind::KW_STATIC: return 2;
    case TokenKind::KW_FINAL: return 3;
    case TokenKind::KW_CONST: return 4;
    default: return 0;
    }
}

/**
 * @brief Indica si un hueco de trivia no lleva nada escrito.
 *
 * `R39b` solo junta un cuerpo cuando los huecos que se lleva por delante
 * estaban VACIOS.  Un comentario ahi dentro es texto del autor, y no hay donde
 * recolocarlo: el cuerpo pasa a ser una sola linea y el comentario se quedaria
 * sin sitio.  Ante eso el cuerpo se deja como estaba.
 *
 * @param trivia El hueco.
 * @return Cierto si solo tiene blancos.
 */
bool blank_gap(std::string_view trivia) {
    for (const char c : trivia)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    return true;
}

/**
 * @brief Indica si la `(` en @p lparen abre la lista de una DECLARACION.
 *
 * Lo que distingue `i64 add(i64 a, i64 b) {` de `if (cond) {` es lo que hay
 * DELANTE del parentesis: una declaracion lleva ahi su nombre -- o el `>` que
 * cierra sus parametros de tipo --, y una cabecera de control lleva su palabra
 * clave.  Como todas esas palabras SON palabras clave, basta con exigir un
 * nombre y no hay que enumerarlas.
 *
 * La excepcion es `spawn on(N) { ... }`, donde `on` si es un nombre; lo delata
 * el `spawn` que lo precede.
 *
 * @param pieces Piezas del fuente.
 * @param lparen Indice del `(`.
 * @return Cierto si lo que abre es la lista de parametros de una declaracion.
 */
bool opens_declaration(const std::vector<Piece> &pieces, size_t lparen) {
    if (lparen == 0) return false;
    const TokenKind before = kind_of(pieces[lparen - 1]);
    // `m<T>(...)`: un metodo generico lleva el cierre de sus tipos delante.
    if (before == TokenKind::GT || before == TokenKind::SHR) return true;
    if (before != TokenKind::IDENTIFIER) return false;
    return lparen < 2 || kind_of(pieces[lparen - 2]) != TokenKind::KW_SPAWN;
}

/**
 * @brief Busca el `)` que cierra los parametros de la declaracion de @p lbrace.
 *
 * Normalmente es el token de justo antes.  Puede haber por medio una clausula
 * `where T: A + B`, que no es una palabra clave sino un NOMBRE que se compara
 * por su lexema -- igual que `as` y `only` en los imports --, asi que se
 * reconoce por el nombre y no por su categoria de token.
 *
 * @param pieces Piezas del fuente.
 * @param lbrace Indice del `{`.
 * @return Indice del `)`, o @c SIZE_MAX si delante no hay una lista de
 *         parametros.
 */
size_t params_close(const std::vector<Piece> &pieces, size_t lbrace) {
    if (kind_of(pieces[lbrace - 1]) == TokenKind::RPAREN) return lbrace - 1;
    for (size_t k = lbrace - 1; k-- > 0;) {
        const TokenKind kind = kind_of(pieces[k]);
        if (kind == TokenKind::SEMICOLON || kind == TokenKind::LBRACE ||
            kind == TokenKind::RBRACE)
            break;
        if (kind != TokenKind::RPAREN) continue;
        // Lo que sigue al `)` tiene que ser la clausula y nada mas.
        return (kind_of(pieces[k + 1]) == TokenKind::IDENTIFIER &&
                pieces[k + 1].text == "where")
                   ? k
                   : SIZE_MAX;
    }
    return SIZE_MAX;
}

/**
 * @brief Busca el `(` que abre el `)` de @p rparen.
 *
 * @param pieces Piezas del fuente.
 * @param rparen Indice del `)`.
 * @return Indice del `(`, o @c SIZE_MAX si no se encontro.
 */
size_t matching_lparen(const std::vector<Piece> &pieces, size_t rparen) {
    int depth = 0;
    for (size_t k = rparen; k-- > 0;) {
        const TokenKind kind = kind_of(pieces[k]);
        if (kind == TokenKind::RPAREN) {
            ++depth;
        } else if (kind == TokenKind::LPAREN) {
            if (depth == 0) return k;
            --depth;
        }
    }
    return SIZE_MAX;
}

/**
 * @brief Busca el `}` que cierra el `{` de @p lbrace exigiendo UN solo `;`.
 *
 * Las dos cosas se miran de una pasada porque la segunda decide: un cuerpo con
 * mas de una sentencia no cabe en una expresion, y con `;` anidados dentro --
 * lo que sea que los ponga -- tampoco se quiere arriesgar.
 *
 * @param pieces Piezas del fuente.
 * @param lbrace Indice del `{`.
 * @return Indice del `}` si el cuerpo es exactamente `return <expr> ;`, o
 *         @c SIZE_MAX si no lo es.
 */
size_t single_return_close(const std::vector<Piece> &pieces, size_t lbrace) {
    int depth = 0;
    size_t semis = 0;
    for (size_t k = lbrace + 1; k < pieces.size(); ++k) {
        const TokenKind kind = kind_of(pieces[k]);
        if (kind == TokenKind::LBRACE) {
            ++depth;
        } else if (kind == TokenKind::RBRACE) {
            if (depth == 0)
                return (semis == 1 && k > 0 &&
                        kind_of(pieces[k - 1]) == TokenKind::SEMICOLON)
                           ? k
                           : SIZE_MAX;
            --depth;
        } else if (kind == TokenKind::SEMICOLON) {
            if (++semis > 1) return SIZE_MAX;
        }
    }
    return SIZE_MAX;
}

} // namespace

std::vector<ExprBody> apply_expression_bodies(std::vector<Piece> &pieces) {
    std::vector<ExprBody> done;
    for (size_t i = 0; i + 3 < pieces.size(); ++i) {
        const Piece &brace = pieces[i];
        if (brace.drop || brace.in_string || brace.verbatim) continue;
        if (kind_of(brace) != TokenKind::LBRACE) continue;
        // Cuerpo de una DECLARACION, no un bloque de sentencias.
        if (i == 0) continue;
        const size_t rparen = params_close(pieces, i);
        if (rparen == SIZE_MAX) continue;
        const size_t lparen = matching_lparen(pieces, rparen);
        if (lparen == SIZE_MAX || !opens_declaration(pieces, lparen)) continue;
        /* Y su unica sentencia es un `return` CON valor: `return;` a secas no
         * es una expresion, y un destructor o una funcion `void` que lo use no
         * tiene nada que poner detras del `=>`. */
        if (kind_of(pieces[i + 1]) != TokenKind::KW_RETURN) continue;
        if (kind_of(pieces[i + 2]) == TokenKind::SEMICOLON) continue;
        const size_t close = single_return_close(pieces, i);
        if (close == SIZE_MAX) continue;
        // Nada escrito en los huecos que la juntada se lleva por delante.
        if (!blank_gap(brace.trivia) || !blank_gap(pieces[i + 1].trivia) ||
            !blank_gap(pieces[close].trivia))
            continue;

        /* Las dos llaves se van y el `return` pasa a ser el `=>`.  Su hueco se
         * vacia para que el cuerpo suba a la linea de la firma: la separacion
         * de ahi la decide despues el espaciado, como la de cualquier par de
         * tokens vecinos. */
        ExprBody body;
        body.open = i;
        body.close = close;
        body.gap = pieces[i + 1].trivia;
        pieces[i].drop = true;
        pieces[close].drop = true;
        pieces[i + 1].kind = static_cast<int>(TokenKind::FAT_ARROW);
        pieces[i + 1].text = "=>";
        pieces[i + 1].glued = "=>";
        pieces[i + 1].trivia = std::string_view();
        done.push_back(body);
        i = close;
    }
    return done;
}

std::vector<Rewrite> keep_fitting_expression_bodies(
    std::vector<Piece> &pieces, const std::vector<ExprBody> &bodies,
    const std::vector<Role> &roles, const Layout &measured,
    const FormatOptions &options) {
    std::vector<Rewrite> done;
    done.reserve(bodies.size() * 2);
    /* Las llaves de los cuerpos que se quedan NO basta con marcarlas: hay que
     * QUITARLAS del vector.
     *
     * Marcarlas solo lo respeta quien emite; quien alinea y quien reparte
     * siguen viendolas, y una llave fantasma parte el bloque de columnas de
     * `R83`.  Eso se veia como que formatear DOS veces daba dos ficheros
     * distintos: en la primera pasada la llave seguia ahi partiendo el grupo,
     * y en la segunda ya no existia y el grupo se formaba.  Se van al final,
     * de una vez, para que los indices de @p bodies valgan durante el bucle. */
    std::vector<bool> gone(pieces.size(), false);
    bool any_gone = false;
    for (const ExprBody &b : bodies) {
        /* Lo que mediria la DECLARACION ENTERA -- firma incluida -- en una
         * sola linea.
         *
         * Entera porque lo que se junta ES una linea: si no cabe hay que
         * repartirla, y repartir un `=>` deja la firma colgando de una linea
         * larga, que es peor que el bloque del que venia.  Cabe entera o el
         * cuerpo se queda como estaba.
         *
         * Y se suma token a token en vez de leer la columna del `;` ya medido,
         * que no es lo mismo: el `;` cae donde caiga segun por donde partiera
         * las lineas QUIEN ESCRIBIO el fichero, asi que la respuesta
         * dependeria de eso -- un cuerpo que no cabe, escrito a mano en tres
         * lineas, medio poco y se colo, dejando la primera a 99 columnas --.
         * Los tokens y su separacion son los mismos se escriba como se
         * escriba, y por eso la decision sale igual siempre. */
        size_t start = b.open;
        while (start > 0 &&
               pieces[start].trivia.find('\n') == std::string_view::npos)
            --start;
        uint32_t end = measured.column[start];
        for (size_t k = start; k < b.close; ++k) {
            if (pieces[k].drop) continue;
            if (k == start) {
                end += display_width(pieces[k].text, options.tab_width);
                continue;
            }
            /* Quien va DELANTE saltandose lo que `R39b` se llevo: la llave
             * dropeada sigue en el vector y el espaciado no debe verla. */
            size_t prev = k - 1;
            while (prev > 0 && pieces[prev].drop)
                --prev;
            size_t before = prev > 0 ? prev - 1 : 0;
            while (before > 0 && pieces[before].drop)
                --before;
            const Spacing sp =
                space_between(prev > 0 ? &pieces[before] : nullptr,
                              pieces[prev], pieces[k], roles[prev], roles[k]);
            if (sp != Spacing::None) ++end;
            end += display_width(pieces[k].text, options.tab_width);
        }
        if (end <= options.width) {
            done.push_back(
                {RewriteKind::ExpressionBody, pieces[b.open].offset});
            done.push_back(
                {RewriteKind::ExpressionBody, pieces[b.close].offset});
            gone[b.open] = true;
            gone[b.close] = true;
            any_gone = true;
            continue;
        }
        // No cabe: el cuerpo vuelve a ser el bloque que era.
        pieces[b.open].drop = false;
        pieces[b.close].drop = false;
        pieces[b.open + 1].kind = static_cast<int>(TokenKind::KW_RETURN);
        pieces[b.open + 1].text = "return";
        pieces[b.open + 1].glued = std::string_view();
        pieces[b.open + 1].trivia = b.gap;
    }
    if (any_gone) {
        std::vector<Piece> kept;
        kept.reserve(pieces.size());
        for (size_t k = 0; k < pieces.size(); ++k)
            if (!gone[k]) kept.push_back(pieces[k]);
        pieces = std::move(kept);
    }
    return done;
}

std::vector<Rewrite> apply_token_rules(std::vector<Piece> &pieces) {
    std::vector<Rewrite> done;
    /* Los papeles dicen cual de los dos `>` cierra un generico y cual compara.
     * Sin ellos habria que fiarse de que no hubiera un espacio por medio, que
     * es justo el caso que `R29` viene a arreglar. */
    const std::vector<Role> roles = annotate_roles(pieces);

    for (size_t i = 0; i < pieces.size(); ++i) {
        if (pieces[i].drop) continue;

        /* `R74`: una anotacion sin argumentos se escribe sin parentesis.
         *
         * `@Override()` y `@Override` son la misma anotacion -- comprobado
         * ejecutando las dos --, y los parentesis vacios solo anaden ruido en
         * la linea que mas se lee de un metodo. */
        if (kind_of(pieces[i]) == TokenKind::LPAREN && i >= 2 &&
            i + 1 < pieces.size() &&
            kind_of(pieces[i + 1]) == TokenKind::RPAREN &&
            kind_of(pieces[i - 1]) == TokenKind::IDENTIFIER &&
            kind_of(pieces[i - 2]) == TokenKind::AT) {
            pieces[i].drop = true;
            pieces[i + 1].drop = true;
            done.push_back({RewriteKind::DropEmptyParens, pieces[i].offset});
            ++i;
            continue;
        }

        /* `R29`: dos `>` que cierran genericos anidados se escriben `>>`.
         *
         * El lenguaje acepta las dos formas -- el parser parte el `>>` cuando
         * toca --, y `Caja<Caja<i64>>` es como se escribe en cualquier sitio.
         * Solo cuando el anotador marco los dos como cierre de tipo: dos `>`
         * de comparacion seguidos no existen, pero mas vale no fiarse. */
        if (kind_of(pieces[i]) == TokenKind::GT && i + 1 < pieces.size() &&
            kind_of(pieces[i + 1]) == TokenKind::GT &&
            roles[i] == Role::TightLeft && roles[i + 1] == Role::TightLeft) {
            pieces[i].glued = ">>";
            pieces[i + 1].drop = true;
            done.push_back({RewriteKind::GlueGenericClose, pieces[i].offset});
            ++i;
            continue;
        }

        /* `R42`: los modificadores van en orden -- acceso, `static`, `final`.
         *
         * Se ordenan por intercambios de vecinos, y cada intercambio se
         * declara: asi la comprobacion los sigue uno a uno en vez de tener que
         * entender la ordenacion entera. */
        if (is_modifier(kind_of(pieces[i])) && i + 1 < pieces.size() &&
            is_modifier(kind_of(pieces[i + 1]))) {
            const int a = modifier_rank(kind_of(pieces[i]));
            const int b = modifier_rank(kind_of(pieces[i + 1]));
            if (a > b) {
                // Se intercambia lo que ES cada pieza, no su hueco: la trivia
                // que las precede pertenece a la linea, no a la palabra.
                std::swap(pieces[i].kind, pieces[i + 1].kind);
                std::swap(pieces[i].text, pieces[i + 1].text);
                done.push_back({RewriteKind::SwapModifiers, pieces[i].offset});
            }
        }
    }
    return done;
}

} // namespace fmt
} // namespace vx
