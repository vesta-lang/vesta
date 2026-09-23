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
#include <vector>

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

/// Un tramo de fuente, en bytes.  Solo de ENTRADA.
struct SourceSpan {
    uint32_t offset = 0;
    uint32_t length = 0;
};

/// Lo que se saca de un fuente en una pasada: la huella del fichero y la de
/// cada tramo pedido, en el mismo orden.
struct TokenHashes {
    uint64_t whole = 0;          ///< La del fuente ENTERO.
    std::vector<uint64_t> spans; ///< Una por tramo pedido.
};

/**
 * @brief La huella de varios TRAMOS del mismo fuente, con UNA sola pasada.
 *
 * Existe porque hacerlo tramo a tramo obliga a lexar el fichero una vez por
 * tramo -- cuadratico en el tamanyo del modulo --, y porque la huella de un
 * tramo tiene que salir de la MISMA cuenta que la del fichero: si fueran dos
 * cuentas, dos huellas del mismo texto podrian diferir y nadie se enteraria
 * hasta que una cache sirviera lo que no era.
 *
 * Todo lo que entra es @c const y todo lo que sale se DEVUELVE.  Un parametro
 * de salida por referencia no dice por la firma si se rellena o se anyade, ni
 * si lo que traia cuenta; y una sola lista con los offsets dentro y las huellas
 * fuera seria mitad entrada y mitad salida -- justo lo que el lenguaje separa
 * en su propio eje @c in / @c out / @c inout.
 *
 * @param source     Texto del modulo.
 * @param with_lines Si la LINEA entra en la cuenta.  Ver @ref hash_de_tokens.
 * @param spans      Los tramos, ORDENADOS por offset y sin solaparse.
 * @return La del fichero entero -- la misma que @ref hash_de_tokens -- y una
 *         por tramo.  Cada tramo se CIERRA con la misma marca de fin que el
 *         fichero, asi que uno SIN NINGUN TOKEN dentro da lo mismo que un
 *         fuente vacio: una respuesta que se puede comparar, en vez de un valor
 *         que no produce nadie mas.
 */
TokenHashes hash_tokens_by_span(const std::string &source, bool with_lines,
                                const std::vector<SourceSpan> &spans);

} // namespace vx

#endif // VX_SOURCE_HASH_H
