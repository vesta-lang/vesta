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
 * @file source_hash.cpp
 * @brief Implementacion de @ref vx::hash_de_tokens (ver source_hash.h).
 */

#include "vx/source_hash.h"

#include "vx/diagnostic.h"
#include "vx/lexer.h"
#include "vx/token.h"

namespace vx {

namespace {

constexpr uint64_t kSemilla = 0xCBF29CE484222325ULL;
constexpr uint64_t kPrimo = 0x100000001B3ULL;

/// Mezcla un bloque de bytes en la huella.
inline void mezclar(uint64_t &h, const void *datos, size_t n) noexcept {
    const auto *p = static_cast<const unsigned char *>(datos);
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= kPrimo;
    }
}

/// Mezcla un entero de 64 bits.
inline void mezclar_u64(uint64_t &h, uint64_t v) noexcept {
    mezclar(h, &v, sizeof(v));
}

} // namespace

uint64_t hash_de_tokens(const std::string &fuente, bool con_lineas) {
    /* Los problemas del fuente no son cosa de esta funcion: aqui solo se
     * recorre.  Si el fuente no compila, lo dira quien lo compile; mientras
     * tanto su huella es tan valida como cualquier otra -- lo unico que se
     * necesita es que dos textos que dicen lo mismo den lo mismo. */
    Diagnostics mudo;
    Lexer lex(fuente, "<huella>", mudo);

    uint64_t h = kSemilla;
    for (;;) {
        const Token t = lex.next();
        mezclar_u64(h, static_cast<uint64_t>(t.kind));
        if (t.kind == TokenKind::END_OF_FILE) break;
        /* El lexema, porque dos identificadores distintos son dos programas
         * distintos aunque su categoria sea la misma. */
        if (!t.lexeme.empty()) mezclar(h, t.lexeme.data(), t.lexeme.size());
        /* Y el valor YA INTERPRETADO de los literales: `0x10` y `16` se
         * escriben distinto y son el mismo numero, pero `1.0` y `1.00000001`
         * se escriben casi igual y no lo son.  Va aparte del lexema porque es
         * lo que de verdad acaba en el codigo. */
        if (t.int_val != 0) mezclar_u64(h, t.int_val);
        if (t.flt_val != 0.0) mezclar(h, &t.flt_val, sizeof(t.flt_val));
        if (!t.str_val.empty()) mezclar(h, t.str_val.data(), t.str_val.size());
        if (con_lineas) mezclar_u64(h, static_cast<uint64_t>(t.loc.line));
        /* Un separador entre tokens: sin el, `ab` y `a` `b` -- un identificador
         * contra dos -- podrian mezclar la misma secuencia de bytes. */
        h ^= 0xFFULL;
        h *= kPrimo;
    }
    return h;
}

} // namespace vx
