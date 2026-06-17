/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file ansi.h
 * @brief Utilidades ANSI para salida de color en terminal multiplataforma.
 *
 * Proporciona:
 *   - ansi::init()        : habilita el modo VT en Windows y detecta si stdout
 * es un TTY.
 *   - ansi::is_enabled()  : true si los codigos de color se pueden emitir.
 *   - ansi::c(code)       : devuelve el codigo si los colores estan activos, ""
 * si no.
 *   - ansi::byte_color(b) : color deterministico para un valor de byte (mismo
 * valor = mismo color).
 *   - Constantes de color: RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, RESET, etc.
 *
 * No depende de ninguna libreria de terceros; usa solo WinAPI en Windows y
 * POSIX isatty() en Linux/macOS.
 */

#ifndef ANSI_H
#define ANSI_H

#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ansi {

/**
 * @brief Inicializa el soporte de color del terminal.
 *
 * En Windows intenta habilitar Virtual Terminal Processing en el handle
 * de stdout.  En POSIX comprueba si stdout es un TTY con isatty().
 * El resultado se cachea: multiples llamadas son baratas.
 *
 * @return true si los codigos ANSI seran interpretados correctamente.
 */
inline bool init() {
    static bool initialized = false;
    static bool enabled = false;
    if (initialized) return enabled;
    initialized = true;

#if defined(_WIN32)
    // Intentar habilitar VT processing para que la consola interprete ANSI
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE || hOut == nullptr)
        return (enabled = false);
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) return (enabled = false);
    enabled = (SetConsoleMode(
                   hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0);
#else
    // POSIX: solo usar color si stdout va a un terminal real (no a fichero o
    // pipe)
    enabled = (isatty(fileno(stdout)) != 0);
#endif
    return enabled;
}

/**
 * @brief Indica si los codigos ANSI de color estan activos.
 * @return true si init() tuvo exito.
 */
inline bool is_enabled() {
    return init();
}

/**
 * @brief Devuelve el codigo ANSI dado si los colores estan activos, o "" si no.
 *
 * @param code Codigo de escape ANSI (p.ej. "\x1B[32m").
 * @return @p code o cadena vacia si los colores estan desactivados.
 */
inline const char *c(const char *code) {
    return is_enabled() ? code : "";
}

// ---- Constantes de color ----

constexpr const char *RESET = "\x1B[0m"; ///< Restaurar todos los atributos
constexpr const char *BOLD = "\x1B[1m";  ///< Negrita
constexpr const char *DIM = "\x1B[2m";   ///< Atenuado
constexpr const char *ITALIC =
    "\x1B[3m"; ///< Cursiva (soporte variable por terminal)
constexpr const char *UNDERLINE = "\x1B[4m"; ///< Subrayado

// Colores estandar (foreground)
constexpr const char *BLACK = "\x1B[30m";   ///< Negro
constexpr const char *RED = "\x1B[31m";     ///< Rojo
constexpr const char *GREEN = "\x1B[32m";   ///< Verde
constexpr const char *YELLOW = "\x1B[33m";  ///< Amarillo
constexpr const char *BLUE = "\x1B[34m";    ///< Azul
constexpr const char *MAGENTA = "\x1B[35m"; ///< Magenta
constexpr const char *CYAN = "\x1B[36m";    ///< Cian
constexpr const char *WHITE = "\x1B[37m";   ///< Blanco

// Colores brillantes (bright / high-intensity)
constexpr const char *BR_BLACK = "\x1B[90m";   ///< Negro brillante (gris)
constexpr const char *BR_RED = "\x1B[91m";     ///< Rojo brillante
constexpr const char *BR_GREEN = "\x1B[92m";   ///< Verde brillante
constexpr const char *BR_YELLOW = "\x1B[93m";  ///< Amarillo brillante
constexpr const char *BR_BLUE = "\x1B[94m";    ///< Azul brillante
constexpr const char *BR_MAGENTA = "\x1B[95m"; ///< Magenta brillante
constexpr const char *BR_CYAN = "\x1B[96m";    ///< Cian brillante
constexpr const char *BR_WHITE = "\x1B[97m";   ///< Blanco brillante

/**
 * @brief Devuelve un color ANSI deterministico para un valor de byte.
 *
 * El mismo valor de byte siempre devuelve el mismo color, lo que permite
 * distinguir visualmente bytes iguales en un listado de desensamblado.
 * Usa modulo sobre una paleta de 8 colores que funcionan bien en terminales
 * con fondo oscuro y claro.
 *
 * @param b Valor del byte (0x00-0xFF).
 * @return Codigo ANSI de color, o "" si los colores no estan activos.
 */
inline const char *byte_color(uint8_t b) {
    // paleta de 8 colores con buen contraste; usa bright para mayor visibilidad
    static const char *palette[] = {
        "\x1B[91m", // rojo brillante     (0, 8, 16, ...)
        "\x1B[93m", // amarillo brillante (1, 9, 17, ...)
        "\x1B[92m", // verde brillante    (2, 10, 18, ...)
        "\x1B[96m", // cian brillante     (3, 11, 19, ...)
        "\x1B[94m", // azul brillante     (4, 12, 20, ...)
        "\x1B[95m", // magenta brillante  (5, 13, 21, ...)
        "\x1B[33m", // amarillo normal    (6, 14, 22, ...)
        "\x1B[36m", // cian normal        (7, 15, 23, ...)
    };
    return is_enabled() ? palette[b % 8] : "";
}

/**
 * @brief Devuelve un color segun el tipo mnemotecnico de la instruccion.
 *
 * Agrupa instrucciones por categoria:
 *   - Movimiento de datos (mov, xchg, push, pop, movc): amarillo
 *   - Aritmetica/logica (add, sub, mul, div, and, or, xor, not, shl, shr, sar,
 * cmp, inc, dec): verde
 *   - Control de flujo (jmp, callvm, jmpr, callvmr, hlt): rojo/magenta
 *   - Memoria/GC (alloc, free): cian
 *   - VM-especificas (writecur, readcur, calln, etc.): azul brillante
 *   - Desconocida (.db): gris atenuado
 *
 * @param mnemonic Nombre mnemotecnico de la instruccion (puede estar en
 * minusculas).
 * @return Codigo ANSI de color o "" si los colores no estan activos.
 */
inline const char *mnem_color(const char *mnemonic) {
    if (!is_enabled() || !mnemonic || !mnemonic[0]) return "";
    // Comparar prefijo del mnemotecnico para agrupar familias
    auto starts = [&](const char *prefix) {
        int i = 0;
        while (prefix[i] && mnemonic[i] && mnemonic[i] == prefix[i])
            i++;
        return prefix[i] == '\0';
    };
    // control de flujo
    if (starts("hlt")) return "\x1B[91m"; // rojo brillante
    if (starts("jmp") || starts("jcc")) return "\x1B[91m";
    if (starts("callvm") || starts("jmpr") || starts("callvmr"))
        return "\x1B[91m";
    // movimiento de datos
    if (starts("mov") || starts("xchg") || starts("push") || starts("pop"))
        return "\x1B[93m";
    if (starts("movc")) return "\x1B[93m";
    // aritmetica y logica
    if (starts("add") || starts("sub") || starts("mul") || starts("div"))
        return "\x1B[92m";
    if (starts("and") || starts("or") || starts("xor") || starts("not"))
        return "\x1B[92m";
    if (starts("shl") || starts("shr") || starts("sar")) return "\x1B[92m";
    if (starts("cmp") || starts("inc") || starts("dec")) return "\x1B[92m";
    // memoria / GC
    if (starts("alloc") || starts("free") || starts("gc"))
        return "\x1B[96m"; // cian brillante
    // VM-especificas: calln, writecur, readcur, vminfo, etc.
    if (starts("calln") || starts("writecur") || starts("readcur"))
        return "\x1B[94m"; // azul brillante
    if (starts("vminfo")) return "\x1B[94m";
    // desconocida
    if (starts(".db")) return "\x1B[90m"; // gris atenuado
    // resto: blanco normal
    return "\x1B[97m";
}

} // namespace ansi

#endif // ANSI_H
