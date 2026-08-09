/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/source_text.h
 * @brief Como entra un fichero fuente al compilador: en un solo formato.
 *
 * Un fichero de texto puede terminar sus lineas de tres maneras -- `\n` en
 * Unix, `\r\n` en Windows, `\r` a secas en los Mac de antes de 2001 -- y el
 * lenguaje no puede comportarse distinto segun cual sea.  Aqui se decide una
 * vez: todo lo que entra se convierte a `\n`, y de ahi en adelante NADIE mas
 * tiene que acordarse.
 *
 * Que "nadie mas tenga que acordarse" no es comodidad, es la unica forma de que
 * esto no vuelva a pasar.  Ya paso dos veces con la misma causa:
 *
 *   - el analisis del asm tomaba el `\r` del final de linea como si fuera una
 *     instruccion, y salia un aviso que decia "mnemonico(s) no reconocido(s)
 *     ()" -- con la lista vacia, porque el nombre era un retorno de carro y no
 *     se podia ni imprimir.  De paso, el ultimo operando de cada linea se
 *     llamaba `v0\r` y no casaba con ningun registro, asi que lo que ese
 *     operando dijera se perdia en silencio;
 *   - un fuente con `\r` a secas contaba como UNA sola linea, y todos sus
 *     diagnosticos senalaban al sitio equivocado -- que es peor que no
 *     compilar, porque parece que funciona.
 *
 * Los dos son el mismo fallo: cada sitio que parte texto por lineas tenia que
 * acordarse por su cuenta.  Se normaliza en la puerta y se acaba.
 *
 * Es ademas header-only a proposito: lo usan el compilador, el servidor de
 * lenguaje y el preprocesador, que no comparten libreria entre si.
 */

#ifndef VX_SOURCE_TEXT_H
#define VX_SOURCE_TEXT_H

#include <fstream>
#include <string>

namespace vx {

/**
 * @brief Deja un texto con un unico fin de linea: `\n`.
 *
 * @param texto Contenido tal como venia.
 * @return El mismo contenido con `\r\n` y `\r` convertidos a `\n`.
 */
inline std::string normalizar_fin_de_linea(std::string texto) {
    // El caso comun -- ya viene en `\n` -- no copia ni recorre de mas.
    const size_t primero = texto.find('\r');
    if (primero == std::string::npos) return texto;

    /* Se reescribe SOBRE el mismo buffer: la salida nunca es mas larga que la
     * entrada (cada `\r\n` se queda en uno, cada `\r` se queda igual), asi que
     * basta con llevar dos indices y recortar al final. */
    size_t w = primero;
    for (size_t r = primero; r < texto.size(); ++r) {
        const char c = texto[r];
        if (c == '\r') {
            // `\r\n` cuenta como UN fin de linea, no como dos.
            if (r + 1 < texto.size() && texto[r + 1] == '\n') continue;
            texto[w++] = '\n';
            continue;
        }
        texto[w++] = c;
    }
    texto.resize(w);
    return texto;
}

/**
 * @brief Lee un fichero fuente entero, ya con los fines de linea normalizados.
 *
 * Se abre en BINARIO a proposito: dejar que la biblioteca traduzca los finales
 * de linea por su cuenta haria que el contenido dependiera del sistema donde se
 * compila, y con el las posiciones que se citan en los diagnosticos.
 *
 * @param path Ruta del fichero.
 * @param out  Contenido, si se pudo leer.
 * @return @c true si se leyo; @c false si no se pudo abrir.
 */
inline bool leer_fuente(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const std::streamsize sz = f.tellg();
    if (sz < 0) return false;
    f.seekg(0, std::ios::beg);
    std::string s;
    s.resize(static_cast<size_t>(sz));
    if (sz > 0) f.read(&s[0], sz);
    out = normalizar_fin_de_linea(std::move(s));
    return true;
}

} // namespace vx

#endif // VX_SOURCE_TEXT_H
