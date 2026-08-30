/**
 * @file verbatim.cpp
 * @brief Llamadas cuyo argumento es TEXTO, no codigo (`R110`).
 *
 * Una funcion con un parametro de tipo `expr` no recibe el VALOR del argumento
 * sino su texto tal como se escribio.  El caso tipico es un macro que genera
 * codigo:
 *
 * @code
 * comptime string emit(expr code) { ... }
 * string s = emit( print("hola"); );   // s vale ` print("hola"); `
 * @endcode
 *
 * Para el formateador eso cambia las reglas: dentro de esos parentesis los
 * espacios son CONTENIDO.  Quitar uno no reordena el programa, lo reescribe --
 * y la salvaguarda de `fmt.cpp` no puede verlo, porque a nivel de tokens el
 * programa es identico y la diferencia solo aparece al ejecutar el comptime.
 * Se descubrio asi: un compilador de Brainfuck escrito en comptime dejo de
 * compilar al formatearlo, y el unico cambio era un espacio dentro de un
 * `source(...)`.
 *
 * Que se puede saber leyendo un fichero, y que no.  Hay tres vias, y ninguna
 * cablea el nombre de una funcion concreta: un nombre de libreria metido en el
 * compilador se queda viejo en cuanto la libreria cambia, y ademas solo
 * protegeria a esa.
 *
 *   1. Las funciones declaradas AQUI se detectan solas, buscando `expr` en su
 *      lista de parametros.
 *   2. Las importadas de otro modulo no se pueden ver: resolver un import es
 *      trabajo del compilador.  Quien llame -- el compilador o el LSP, que si
 *      lo tienen resuelto -- pasa esos nombres en
 *      @c FormatOptions::raw_capture_names.
 *   3. Y queda una senal que es del LENGUAJE y no de ninguna libreria: un `;`
 *      suelto entre los parentesis de una llamada.  Una expresion Vesta nunca
 *      lleva uno, asi que si aparece, lo de dentro no es una expresion sino
 *      texto.  Cubre `source( p++; )` sin saber que existe `source`.
 */

#include "vx/fmt/fmt_internal.h"

#include "vx/token.h"

namespace vx {
namespace fmt {
namespace {

/// @brief Cierto si la pieza es el token dado.
bool is(const Piece &p, TokenKind k) {
    return p.kind == (int)k;
}

/// @brief Cierto si la pieza es un identificador con ese nombre.
bool is_word(const Piece &p, std::string_view word) {
    return p.kind == (int)TokenKind::IDENTIFIER && p.text == word;
}

} // namespace

/**
 * @brief Nombres declarados en un fichero que capturan el texto del argumento.
 *
 * Se busca el patron de una declaracion: un identificador, un `(`, y dentro
 * un `expr` en posicion de tipo (o sea, seguido del nombre del parametro).
 * No hace falta parsear: `expr` solo puede aparecer asi.
 *
 * Es publica porque el mismo escaneo vale para los OTROS ficheros del
 * proyecto: quien resuelve los imports los pasa por aqui y le da al
 * formateador los nombres que el, mirando un solo fichero, no puede ver.
 */
std::vector<std::string> capture_names_in(const std::vector<Piece> &pieces) {
    std::vector<std::string> names;
    for (size_t i = 1; i + 2 < pieces.size(); ++i) {
        if (!is(pieces[i], TokenKind::LPAREN)) continue;
        if (pieces[i - 1].kind != (int)TokenKind::IDENTIFIER) continue;
        // Recorrer la lista de parametros hasta su cierre buscando `expr`.
        int depth = 1;
        bool captures = false;
        for (size_t j = i + 1; j < pieces.size() && depth > 0; ++j) {
            if (is(pieces[j], TokenKind::LPAREN))
                ++depth;
            else if (is(pieces[j], TokenKind::RPAREN))
                --depth;
            else if (depth == 1 && is_word(pieces[j], "expr") &&
                     j + 1 < pieces.size() &&
                     pieces[j + 1].kind == (int)TokenKind::IDENTIFIER)
                captures = true;
        }
        if (captures) names.emplace_back(pieces[i - 1].text);
    }
    return names;
}

/**
 * @brief Marca como literal el interior de las llamadas que capturan texto.
 *
 * @param pieces Piezas del fichero; se les pone @c Piece::verbatim.
 * @param extra Nombres que el llamador sabe que capturan (los importados).
 */
void mark_verbatim_calls(std::vector<Piece> &pieces,
                         const std::vector<std::string> &extra) {
    std::vector<std::string> names = capture_names_in(pieces);
    names.insert(names.end(), extra.begin(), extra.end());

    const auto declared = [&names](std::string_view w) {
        for (const std::string &n : names)
            if (n == w) return true;
        return false;
    };

    for (size_t i = 0; i + 1 < pieces.size(); ++i) {
        if (pieces[i].kind != (int)TokenKind::IDENTIFIER) continue;
        if (!is(pieces[i + 1], TokenKind::LPAREN)) continue;
        // Recorrer la llamada hasta su cierre, anotando por el camino las dos
        // cosas que deciden: si es la DECLARACION (lleva un `expr` dentro, y
        // entonces el argumento aun no existe) y si el argumento lleva un `;`,
        // que en una expresion no cabe.
        int depth = 0;
        size_t j = i + 1;
        bool is_decl = false, has_statement = false;
        for (; j < pieces.size(); ++j) {
            if (is(pieces[j], TokenKind::LPAREN))
                ++depth;
            else if (is(pieces[j], TokenKind::RPAREN)) {
                if (--depth == 0) break;
            } else if (depth == 1 && is_word(pieces[j], "expr"))
                is_decl = true;
            else if (is(pieces[j], TokenKind::SEMICOLON))
                has_statement = true;
        }
        if (is_decl) continue;
        if (!declared(pieces[i].text) && !has_statement) continue;
        /* Todo lo que va entre los parentesis es texto del autor.
         *
         * El cierre entra en el bucle aunque el `)` no sea del argumento: cada
         * pieza arrastra la trivia que la PRECEDE, y la del `)` es el ultimo
         * espacio del texto capturado.  Dejandolo fuera se perdia ese espacio
         * y `source( x );` capturaba ` x` en vez de ` x `.  El `(` de apertura
         * si se queda fuera: su trivia es lo que hay ANTES de la llamada. */
        for (size_t k = i + 2; k <= j && k < pieces.size(); ++k)
            pieces[k].verbatim = true;
        i = j;
    }
}

} // namespace fmt
} // namespace vx
