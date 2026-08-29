/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/ansi_names.h
 * @brief Los nombres que Vesta reconoce como color y estilo de terminal.
 *
 * `RED`, `BOLD`, `CLEAR_SCREEN` y los demas son identificadores que el
 * lenguaje conoce de fabrica: se escriben como una constante cualquiera y
 * valen la secuencia de escape que el terminal entiende.  Nada de esto llega
 * al programa como codigo -- la secuencia se resuelve al compilar y queda
 * como texto.
 *
 * Existe este fichero porque el dato lo necesitan TRES sitios que no se ven
 * entre si: el que decide que el nombre existe, el que lo baja como valor, y
 * el que lo baja dentro de un texto que se imprime.  Con una lista por sitio,
 * anñadir un color y olvidarse de uno de los tres da un lenguaje incoherente
 * -- un nombre que vale como valor pero no dentro de `${}`, o al reves -- y
 * eso no lo detecta ningun compilador.  Aqui la lista es UNA.
 *
 * No confundir con `util/ansi.h`, que son las secuencias que usa el propio
 * compilador para pintar SU salida.  Aquello es como se ve el compilador;
 * esto es lo que el lenguaje ofrece a quien escribe en Vesta.
 */

#ifndef VX_ANSI_NAMES_H
#define VX_ANSI_NAMES_H

#include <cstddef>
#include <string_view>

namespace vx {

/**
 * @brief Un nombre del lenguaje y la secuencia que vale.
 */
struct AnsiName {
    const char *name; ///< Como se escribe en Vesta.
    const char *seq;  ///< Los bytes que van al terminal.
};

/**
 * @brief Todos ellos, en un array plano.
 *
 * Plano y no un mapa a proposito: son treinta y tantos, se recorren solo al
 * compilar, y buscados asi caben en una linea de cache.  El orden es el de la
 * norma -- frente, brillantes, fondo, estilos -- para que anñadir uno sea
 * evidente donde va.
 */
inline constexpr AnsiName kAnsiNames[] = {
    // Frente (SGR 30..37).
    {"BLACK", "\x1b[30m"},
    {"RED", "\x1b[31m"},
    {"GREEN", "\x1b[32m"},
    {"YELLOW", "\x1b[33m"},
    {"BLUE", "\x1b[34m"},
    {"MAGENTA", "\x1b[35m"},
    {"CYAN", "\x1b[36m"},
    {"WHITE", "\x1b[37m"},
    // Frente brillante (SGR 90..97).
    {"BR_BLACK", "\x1b[90m"},
    {"BR_RED", "\x1b[91m"},
    {"BR_GREEN", "\x1b[92m"},
    {"BR_YELLOW", "\x1b[93m"},
    {"BR_BLUE", "\x1b[94m"},
    {"BR_MAGENTA", "\x1b[95m"},
    {"BR_CYAN", "\x1b[96m"},
    {"BR_WHITE", "\x1b[97m"},
    // Fondo (SGR 40..47).
    {"BG_BLACK", "\x1b[40m"},
    {"BG_RED", "\x1b[41m"},
    {"BG_GREEN", "\x1b[42m"},
    {"BG_YELLOW", "\x1b[43m"},
    {"BG_BLUE", "\x1b[44m"},
    {"BG_MAGENTA", "\x1b[45m"},
    {"BG_CYAN", "\x1b[46m"},
    {"BG_WHITE", "\x1b[47m"},
    // Estilos y vuelta a lo normal.
    {"BOLD", "\x1b[1m"},
    {"DIM", "\x1b[2m"},
    {"ITALIC", "\x1b[3m"},
    {"UNDERLINE", "\x1b[4m"},
    {"BLINK", "\x1b[5m"},
    {"REVERSE", "\x1b[7m"},
    {"RESET", "\x1b[0m"},
    // Pantalla, que hacen falta para cualquier cosa a pantalla completa.
    {"CLEAR_SCREEN", "\x1b[2J"},
    {"CURSOR_HOME", "\x1b[H"},
};

/// @brief Cuantos son.
inline constexpr size_t kAnsiNameCount =
    sizeof(kAnsiNames) / sizeof(kAnsiNames[0]);

/// @brief Que ningun nombre este dos veces (el segundo seria inalcanzable).
constexpr bool ansi_names_are_unique() noexcept {
    for (size_t i = 0; i < kAnsiNameCount; ++i)
        for (size_t j = i + 1; j < kAnsiNameCount; ++j)
            if (std::string_view(kAnsiNames[i].name) == kAnsiNames[j].name)
                return false;
    return true;
}

/// @brief Que toda secuencia empiece por ESC, que es lo que la hace secuencia.
constexpr bool ansi_sequences_are_escapes() noexcept {
    for (const AnsiName &a : kAnsiNames)
        if (a.seq[0] != '\x1b') return false;
    return true;
}

// Se comprueban al compilar: un nombre repetido dejaria el segundo muerto, y
// una secuencia sin ESC saldria por pantalla como texto suelto.  Las dos son
// faciles de colar anñadiendo un color a mano.
static_assert(ansi_names_are_unique(), "hay un nombre de color repetido");
static_assert(ansi_sequences_are_escapes(),
              "hay una secuencia que no empieza por ESC");

/**
 * @brief La secuencia de un nombre, o nada si ese nombre no es de estos.
 *
 * @param name El identificador tal cual se escribio.
 * @return Los bytes para el terminal, o @c nullptr si no es un nombre magico.
 */
inline constexpr const char *ansi_sequence_for(std::string_view name) noexcept {
    for (const AnsiName &a : kAnsiNames)
        if (name == a.name) return a.seq;
    return nullptr;
}

} // namespace vx

#endif // VX_ANSI_NAMES_H
