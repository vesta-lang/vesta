/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/util/test_report.h
 * @brief Lo comun a los tests que ademas son INFORMES: color, veredictos y
 *        recuento.
 *
 * Varios tests de este arbol no son solo un si o un no: son el estado de una
 * parte del compilador que se lee entera -- que instrucciones tienen efectos
 * declarados, que se comprobo, que quedo pendiente --.  Eso pide imprimir el
 * proceso, no solo el resultado, y pide color para recorrerlo con la vista.
 *
 * Y pide estar UNA vez.  Estaba copiado en dos tests, que es como empiezan a
 * separarse: uno apaga el color con NO_COLOR y el otro no, uno imprime lo que
 * pasa y el otro solo lo que falla, y al final dos informes del mismo arbol no se
 * leen igual.
 *
 * Cabecera sola, sin biblioteca que enlazar: un test de este arbol se compila con
 * un `g++` y sus objetos, y anadirle una dependencia de enlace por unas cadenas
 * de color seria peor que la duplicacion.
 */
#ifndef TESTS_UTIL_TEST_REPORT_H
#define TESTS_UTIL_TEST_REPORT_H

#include <cstdio>
#include <cstdlib>
#include <string>

namespace tests {

/**
 * @brief Si la salida lleva color.
 *
 * Se apaga con @c NO_COLOR, que es la convencion de todas las herramientas: un
 * informe redirigido a un fichero o leido por otro programa no debe llevar
 * escapes dentro.
 */
inline bool color_enabled() {
    static const bool v = std::getenv("NO_COLOR") == nullptr;
    return v;
}

/// Devuelve @p code si hay color, o la cadena vacia si no: asi el mismo `printf`
/// sirve para los dos casos sin duplicar el formato.
inline const char *ansi(const char *code) {
    return color_enabled() ? code : "";
}

/// @name Colores, nombrados por lo que SIGNIFICAN donde se pueda.
/// @{
inline const char *dim() { return ansi("\033[90m"); }     ///< detalle secundario
inline const char *red() { return ansi("\033[31m"); }     ///< algo esta mal
inline const char *green() { return ansi("\033[32m"); }   ///< algo esta bien
inline const char *amber() { return ansi("\033[33m"); }   ///< pendiente, no roto
inline const char *cyan() { return ansi("\033[36m"); }    ///< un grupo o seccion
inline const char *bold() { return ansi("\033[1m"); }     ///< titulo
inline const char *reset() { return ansi("\033[0m"); }    ///< vuelve a lo normal
/// @}

/**
 * @brief Recuento de un informe: lo correcto, lo pendiente y lo roto.
 *
 * Los tres van separados a proposito.  "Pendiente" no es "roto": es trabajo
 * identificado que aun no se ha hecho, y mezclarlos obliga a elegir entre dejar
 * el test en rojo permanente -- que se acaba ignorando, y entonces el dia que se
 * rompa otra cosa nadie lo mira -- o borrar el caso, que es peor porque entonces
 * nadie sabe que falta.
 */
struct Tally {
    int passed = 0;
    int pending = 0;
    int failed = 0;

    /// Codigo de salida: solo los FALLOS lo ponen a uno.
    int exit_code() const { return failed == 0 ? 0 : 1; }
};

/// Titulo del informe.
inline void title(const char *text) {
    std::printf("%s[%s]%s\n", bold(), text, reset());
}

/// Cabecera de seccion, con lo que la describa.
inline void section(const char *name, const char *detail = "") {
    std::printf("\n%s== %s ==%s %s%s%s\n", bold(), name, reset(), dim(), detail,
                reset());
}

/// Una comprobacion que salio bien.  Se imprime TAMBIEN lo correcto: un informe
/// que solo ensena los fallos no dice que se comprobo, y entonces no se puede
/// distinguir "todo bien" de "no se miro".
inline void pass(Tally &t, const char *what, const char *detail = "") {
    std::printf("  %sok%s   %-28s %s%s%s\n", green(), reset(), what, dim(), detail,
                reset());
    ++t.passed;
}

/// Algo identificado que aun no esta hecho.  Lleva SIEMPRE su motivo: un
/// pendiente sin motivo es indistinguible de un caso olvidado.
inline void pending(Tally &t, const char *what, const char *why) {
    std::printf("  %spend%s %-28s %s%s%s\n", amber(), reset(), what, dim(), why,
                reset());
    ++t.pending;
}

/// Algo que esta mal.  Se dice lo esperado, lo real y POR QUE se esperaba eso:
/// sin el porque, quien lo lea manana no sabe si el equivocado es el test o el
/// codigo.
inline void fail(Tally &t, const char *what, const std::string &expected,
                 const std::string &actual, const char *why) {
    std::printf("  %sFAIL%s  %-28s\n", red(), reset(), what);
    std::printf("        esperado: %s%s%s\n", green(), expected.c_str(), reset());
    std::printf("        real:     %s%s%s\n", red(), actual.c_str(), reset());
    std::printf("        porque:   %s%s%s\n", dim(), why, reset());
    ++t.failed;
}

/// Resumen final con los tres recuentos.
inline void summary(const char *what, const Tally &t) {
    const char *tint =
        t.failed != 0 ? red() : (t.pending != 0 ? amber() : green());
    std::printf("\n%s[%s]%s %s%d correctas%s, %s%d pendientes%s, %s%d fallos%s "
                "-> %s%s%s\n",
                bold(), what, reset(), green(), t.passed, reset(), amber(),
                t.pending, reset(), red(), t.failed, reset(), tint,
                t.failed == 0 ? "OK" : "CON FALLOS", reset());
}

} // namespace tests

#endif // TESTS_UTIL_TEST_REPORT_H
