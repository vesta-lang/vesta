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
 * @file source_hash.h
 * @brief La identidad de un fuente para la cache: lo que DICE, no como esta
 *        escrito.
 */

#ifndef VX_SOURCE_HASH_H
#define VX_SOURCE_HASH_H

#include <cstdint>
#include <string>

namespace vx {

/**
 * @brief Huella del contenido con SIGNIFICADO de un fuente: sus tokens.
 *
 * La cache de modulos se keyeaba por los bytes del fichero, asi que anadir un
 * comentario, reindentar o cambiar los finales de linea obligaba a recompilar
 * un modulo que dice exactamente lo mismo.  Medido en el banco: tocar un
 * comentario costaba mas que cambiar el cuerpo de una funcion.
 *
 * Lo que entra en la huella es la secuencia de tokens -- categoria, lexema y el
 * valor ya interpretado de los literales --, que es justo lo que el resto del
 * compilador va a leer.  Lo que NO entra son los comentarios ni el espaciado,
 * porque no llegan a producir nada.
 *
 * @param fuente        Texto del modulo.
 * @param con_lineas    Incluir la LINEA de cada token.  Hace falta cuando el
 *                      artefacto va a llevar informacion de depuracion: alli el
 *                      resultado si depende de en que linea esta cada cosa, y
 *                      un comentario insertado en medio las desplaza todas.
 * @return Huella FNV-1a de 64 bits.  Dos fuentes que solo difieren en
 *         comentarios o espaciado dan la MISMA (salvo @p con_lineas).
 */
uint64_t hash_de_tokens(const std::string &fuente, bool con_lineas);

} // namespace vx

#endif // VX_SOURCE_HASH_H
