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
 * @file width.cpp
 * @brief Cuantas columnas ocupa un texto.  Ver `width.h` para el porque.
 *
 * Las dos tablas son rangos ORDENADOS y se buscan por biseccion: el caso comun
 * -- ASCII -- ni siquiera llega a mirarlas, porque se corta antes con una sola
 * comparacion.  Un texto de codigo es ASCII en su inmensa mayoria y esto se
 * llama por cada linea que se mide.
 */

#include "vx/fmt/width.h"

#include <algorithm>

namespace vx {
namespace fmt {
namespace {

/// Un rango cerrado de puntos de codigo.
struct Range {
    uint32_t lo;
    uint32_t hi;
};

/**
 * Los que NO ocupan sitio: se pintan encima del caracter anterior.
 *
 * Acentos combinantes, marcas de las escrituras indias, selectores de
 * variacion, y los juntadores de ancho cero -- el `ZWJ` (U+200D) es el que une
 * los emojis compuestos como la familia o la mujer programadora: sin contarlo
 * cero, un solo emoji se contaria como cuatro.
 */
constexpr Range kZeroWidth[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD},   {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC},   {0x0711, 0x0711},
    {0x0730, 0x074A}, {0x07A6, 0x07B0}, {0x0816, 0x0819},   {0x08E3, 0x0903},
    {0x093A, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D},   {0x0951, 0x0957},
    {0x0A01, 0x0A02}, {0x0B01, 0x0B01}, {0x0C3E, 0x0C40},   {0x0E31, 0x0E31},
    {0x0E34, 0x0E3A}, {0x0EB1, 0x0EB1}, {0x0F71, 0x0F7E},   {0x102D, 0x1030},
    {0x1160, 0x11FF}, {0x135D, 0x135F}, {0x1712, 0x1714},   {0x17B4, 0x17D3},
    {0x180B, 0x180E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF},   {0x200B, 0x200F},
    {0x202A, 0x202E}, {0x2060, 0x2064}, {0x20D0, 0x20F0},   {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF},
};

/**
 * Los que ocupan el DOBLE.
 *
 * Las escrituras de Asia oriental -- chino, japones, coreano -- se componen en
 * una rejilla donde cada signo mide dos columnas, y los emojis siguen la misma
 * convencion en los terminales.
 */
constexpr Range kWide[] = {
    {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},
    {0x23E9, 0x23EC},   {0x25FD, 0x25FE},   {0x2614, 0x2615},
    {0x2648, 0x2653},   {0x267F, 0x267F},   {0x2693, 0x2693},
    {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},
    {0x2705, 0x2705},   {0x270A, 0x270B},   {0x2728, 0x2728},
    {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},
    {0x2757, 0x2757},   {0x2795, 0x2797},   {0x27B0, 0x27B0},
    {0x27BF, 0x27BF},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},   {0x2E80, 0x303E},   {0x3041, 0x33FF},
    {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},
    {0xA960, 0xA97F},   {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},
    {0xFE10, 0xFE19},   {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},
    {0xFFE0, 0xFFE6},   {0x16FE0, 0x16FE4}, {0x17000, 0x18AFF},
    {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F320},
    {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0},
    {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440},
    {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E},
    {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

/**
 * @brief Busca un punto de codigo en una tabla de rangos, por biseccion.
 * @param table Tabla ordenada por @c lo.
 * @param n     Cuantos rangos tiene.
 * @param cp    Punto de codigo.
 * @return Cierto si cae en alguno.
 */
bool in_table(const Range *table, size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cp < table[mid].lo) {
            hi = mid;
        } else if (cp > table[mid].hi) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

} // namespace

uint32_t decode_utf8(std::string_view text, size_t &pos) {
    if (pos >= text.size()) {
        ++pos;
        return 0;
    }
    const unsigned char b0 = static_cast<unsigned char>(text[pos]);
    if (b0 < 0x80) { // ASCII: el caso comun, resuelto sin mirar nada mas
        ++pos;
        return b0;
    }

    // Cuantos bytes dice el byte lider que tiene la secuencia.
    uint32_t cp = 0;
    size_t len = 0;
    if ((b0 & 0xE0) == 0xC0) {
        cp = b0 & 0x1Fu;
        len = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
        cp = b0 & 0x0Fu;
        len = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
        cp = b0 & 0x07u;
        len = 4;
    } else {
        // Byte de continuacion suelto o lider invalido: se cuenta como uno y se
        // sigue.  Pararse aqui seria dejar de formatear un fichero entero por
        // un byte mal guardado.
        ++pos;
        return b0;
    }

    if (pos + len > text.size()) { // secuencia cortada por el final
        ++pos;
        return b0;
    }
    for (size_t k = 1; k < len; ++k) {
        const unsigned char bk = static_cast<unsigned char>(text[pos + k]);
        if ((bk & 0xC0) != 0x80) { // continuacion que no lo es
            ++pos;
            return b0;
        }
        cp = (cp << 6) | (bk & 0x3Fu);
    }
    pos += len;
    return cp;
}

uint32_t codepoint_width(uint32_t cp) {
    // ASCII imprimible: una columna, y se decide con una sola comparacion.
    if (cp >= 0x20 && cp < 0x7F) return 1;
    // Los de control no ocupan: un `\n` o un `\r` no pintan nada en la linea.
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if (in_table(kZeroWidth, sizeof(kZeroWidth) / sizeof(Range), cp)) return 0;
    if (in_table(kWide, sizeof(kWide) / sizeof(Range), cp)) return 2;
    return 1;
}

uint32_t display_width(std::string_view text, uint32_t tab_width,
                       uint32_t start) {
    if (tab_width == 0) tab_width = 1; // no dividir por cero jamas
    uint32_t column = start;
    size_t pos = 0;
    while (pos < text.size()) {
        // El tabulador no mide lo mismo siempre: avanza a la siguiente PARADA.
        if (text[pos] == '\t') {
            column = ((column / tab_width) + 1) * tab_width;
            ++pos;
            continue;
        }
        column += codepoint_width(decode_utf8(text, pos));
    }
    return column - start;
}

} // namespace fmt
} // namespace vx
