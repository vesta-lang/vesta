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
 * @file trivia.cpp
 * @brief Recupera del texto la trivia que el lexer tira.
 *
 * Va aparte del resto del formateador a proposito: esto NO decide formato, solo
 * reconstruye lo que el compilador descarta -- espacios, saltos y comentarios
 * --.  Es la pieza de la que depende que un comentario no se pierda al
 * formatear, y la unica que toca el lexer.
 *
 * El truco es que no hace falta guardar nada: cada token lleva su `offset` y su
 * `length`, asi que lo que hay entre el final de uno y el principio del
 * siguiente es, literalmente, lo que se escribio ahi.  Por eso el compilador no
 * paga un solo ciclo por tener esta funcionalidad.
 */

#include "vx/fmt/fmt.h"

#include "vx/diagnostic.h"
#include "vx/lexer.h"

namespace vx {
namespace fmt {

std::vector<Piece> scan_pieces(std::string_view source,
                               const std::string &filename,
                               std::string_view &tail, std::string &code) {
    std::vector<Piece> pieces;
    tail = std::string_view();
    code.clear();
    if (source.empty()) return pieces;

    Diagnostics diags;
    // El lexer toma el fuente por valor; es la unica copia del proceso y la
    // hace el, no nosotros.
    Lexer lexer(std::string(source), filename, diags);

    /* Un fichero no puede tener mas tokens que bytes, asi que este tope solo
     * salta si el lexer deja de avanzar -- y entonces vale mas parar que
     * quedarse colgado. */
    const size_t limit = source.size() + 2;
    // Reservar por lo bajo: un token cada seis bytes es una estimacion prudente
    // para codigo real, y ahorra la mayoria de las realocaciones.
    pieces.reserve(source.size() / 6 + 8);

    uint32_t cursor = 0; // primer byte aun no emitido
    int istr_depth = 0;  // cadenas interpoladas abiertas
    for (size_t i = 0; i < limit; ++i) {
        const Token t = lexer.next();
        if (t.kind == TokenKind::END_OF_FILE) break;

        /* La cuenta de cadenas interpoladas se lleva AQUI porque aqui se ven
         * todos los tokens, incluidos los sinteticos que las abren y cierran.
         * Mas adelante ya no estan, y sin esto quien formatea no puede saber
         * que un trozo es contenido de una cadena y no espacio entre tokens. */
        if (t.kind == TokenKind::ISTR_BEGIN) ++istr_depth;

        /* Un token SINTETICO no sale de ningun texto: lo fabrica el lexer al
         * partir una cadena interpolada en trozos.  No ocupa sitio en el
         * fuente, asi que emitirlo duplicaria bytes.  Se reconoce por tener
         * longitud cero. */
        if (t.loc.length == 0) {
            if (t.kind == TokenKind::ISTR_END && istr_depth > 0) --istr_depth;
            continue;
        }

        // Lo que sigue solo puede pasar si el lexer se equivoca.  Se comprueba
        // porque el precio de no hacerlo es corromper el fichero del usuario.
        if (t.loc.offset < cursor) {
            code = "VXF001";
            return {};
        }
        if (static_cast<size_t>(t.loc.offset) + t.loc.length > source.size()) {
            code = "VXF002";
            return {};
        }

        Piece p;
        p.trivia = source.substr(cursor, t.loc.offset - cursor);
        p.text = source.substr(t.loc.offset, t.loc.length);
        p.offset = t.loc.offset;
        p.kind = static_cast<int>(t.kind);
        p.in_string = istr_depth > 0;
        pieces.push_back(p);
        cursor = t.loc.offset + t.loc.length;
        if (t.kind == TokenKind::ISTR_END && istr_depth > 0) --istr_depth;
    }
    // Lo que sobra tras el ultimo token: normalmente, el salto de linea final.
    tail = source.substr(cursor);
    return pieces;
}

} // namespace fmt
} // namespace vx
