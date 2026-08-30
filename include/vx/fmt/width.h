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
 * @file width.h
 * @brief Cuantas COLUMNAS ocupa un texto en pantalla.
 *
 * Es la pieza de la que dependen las dos reglas que miden: el limite de linea
 * (`R3`) y la alineacion en columnas (`R84`).  Y no es contar bytes ni contar
 * caracteres, que son las dos formas de equivocarse:
 *
 *   - `"你好"` (dos ideogramas) son SEIS bytes, DOS caracteres y
 *     CUATRO columnas: en las escrituras de Asia oriental cada ideograma ocupa
 *     el doble.
 *   - Un emoji son cuatro bytes, un caracter y dos columnas.
 *   - Una `e` con acento son dos bytes, un caracter y UNA columna.
 *   - Un acento combinante son dos bytes, un caracter y CERO columnas: se pinta
 *     encima de la letra anterior.
 *
 * Contando bytes, un comentario en chino desalinea la columna entera.  Contando
 * caracteres, la desalinea al reves.  Hay que contar columnas.
 *
 * ROBUSTEZ: un fichero puede traer UTF-8 mal formado -- un editor que guardo a
 * medias, un `#include` de un fichero en otra codificacion --.  Aqui eso NO es
 * excepcional: cada byte invalido cuenta como una columna y se sigue.  Nunca se
 * lanza, nunca se lee fuera del buffer y nunca se deja de avanzar.
 */

#ifndef VX_FMT_WIDTH_H
#define VX_FMT_WIDTH_H

#include <cstdint>
#include <string_view>

namespace vx {
namespace fmt {

/**
 * @brief Decodifica un punto de codigo UTF-8.
 *
 * @param text  Texto.
 * @param pos   [in,out] posicion; avanza a la siguiente secuencia.  Siempre
 *              avanza al menos un byte, aunque la secuencia sea invalida.
 * @return El punto de codigo, o el byte crudo si la secuencia no es valida.
 */
uint32_t decode_utf8(std::string_view text, size_t &pos);

/**
 * @brief Columnas que ocupa un punto de codigo.
 *
 * @param cp Punto de codigo.
 * @return 0 para los que se pintan encima del anterior, 2 para los de ancho
 *         doble (Asia oriental y emojis), 1 para el resto.
 */
uint32_t codepoint_width(uint32_t cp);

/**
 * @brief Columnas que ocupa un texto.
 *
 * @param text      Texto en UTF-8.
 * @param tab_width Cuanto mide un tabulador (`R3`: cuatro).
 * @param start     Columna en la que empieza el texto; importa porque un
 *                  tabulador avanza hasta la siguiente PARADA, no una anchura
 *                  fija.
 * @return Columnas ocupadas.
 */
uint32_t display_width(std::string_view text, uint32_t tab_width = 4,
                       uint32_t start = 0);

} // namespace fmt
} // namespace vx

#endif // VX_FMT_WIDTH_H
