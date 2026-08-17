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
 * declarados, que se comprobo y que quedo pendiente --.  Eso pide imprimir el
 * proceso, no solo el resultado, y pide color para poder recorrerlo con la vista.
 *
 * Y pide estar UNA vez.  Estaba copiado en dos tests, que es como empiezan a
 * separarse: uno apaga el color con NO_COLOR y el otro no, uno imprime lo que
 * pasa y el otro solo lo que falla, y al final dos informes del mismo arbol no se
 * leen igual.
 *
 * Cabecera sola, sin biblioteca que enlazar: un test de este arbol se compila con
 * un `g++` y sus objetos, y anadirle una dependencia de enlace por unas cadenas de
 * color seria peor que la duplicacion.
 */
#ifndef TESTS_UTIL_TEST_REPORT_H
#define TESTS_UTIL_TEST_REPORT_H

#include <cstdarg>
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
inline bool color_activo() {
    static const bool v = std::getenv("NO_COLOR") == nullptr;
    return v;
}

/// Devuelve @p code si hay color, o la cadena vacia si no.  Asi el mismo
/// `printf` sirve para los dos casos sin duplicar el formato.
inline const char *c(const char *code) { return color_activo() ? code : ""; }

/// @name Colores.  Nombres por lo que SIGNIFICAN donde se pueda.
/// @{
inline const char *gris() { return c("\033[90m"); }   ///< detalle secundario
inline const char *rojo() { return c("\033[31m"); }   ///< algo esta mal
inline const char *verde() { return c("\033[32m"); }  ///< algo esta bien
inline const char *ambar() { return c("\033[33m"); }  ///< pendiente, no roto
inline const char *azul() { return c("\033[36m"); }   ///< un grupo, una seccion
inline const char *fuerte() { return c("\033[1m"); }  ///< titulo
inline const char *fin() { return c("\033[0m"); }     ///< vuelve a lo normal
/// @}

/**
 * @brief Recuento de un informe: lo correcto, lo pendiente y lo roto.
 *
 * Los tres van separados a proposito.  "Pendiente" no es "roto": es trabajo
 * identificado que todavia no se ha hecho, y mezclarlos obliga a elegir entre
 * dejar el test en rojo permanente -- que se acaba ignorando, y entonces el dia
 * que se rompa otra cosa nadie lo mira -- o borrar el caso, que es peor porque
 * entonces nadie sabe que falta.
 */
struct Recuento {
    int ok = 0;
    int pendientes = 0;
    int fallos = 0;

    /// Codigo de salida: solo los FALLOS lo ponen a uno.
    int codigo() const { return fallos == 0 ? 0 : 1; }
};

/// Titulo del informe.
inline void titulo(const char *texto) {
    std::printf("%s[%s]%s\n", fuerte(), texto, fin());
}

/// Cabecera de una seccion, con lo que la describa.
inline void seccion(const char *nombre, const char *detalle = "") {
    std::printf("\n%s== %s ==%s %s%s%s\n", fuerte(), nombre, fin(), gris(),
                detalle, fin());
}

/// Una comprobacion que salio bien.  Se imprime TAMBIEN lo correcto: un informe
/// que solo ensena los fallos no dice que se comprobo, y entonces no se puede
/// distinguir "todo bien" de "no se miro".
inline void ok(Recuento &r, const char *que, const char *detalle = "") {
    std::printf("  %sok%s   %-28s %s%s%s\n", verde(), fin(), que, gris(), detalle,
                fin());
    ++r.ok;
}

/// Algo identificado que aun no esta hecho.  Lleva SIEMPRE su motivo: un
/// pendiente sin motivo es indistinguible de un caso olvidado.
inline void pendiente(Recuento &r, const char *que, const char *motivo) {
    std::printf("  %spend%s %-28s %s%s%s\n", ambar(), fin(), que, gris(), motivo,
                fin());
    ++r.pendientes;
}

/// Algo que esta mal.  Se dice lo esperado, lo real y por que se esperaba eso --
/// sin el porque, quien lo lea manana no sabe si el test o el codigo es el
/// equivocado.
inline void fallo(Recuento &r, const char *que, const std::string &esperado,
                  const std::string &real, const char *porque) {
    std::printf("  %sFALLA%s %-28s\n", rojo(), fin(), que);
    std::printf("        esperado: %s%s%s\n", verde(), esperado.c_str(), fin());
    std::printf("        real:     %s%s%s\n", rojo(), real.c_str(), fin());
    std::printf("        porque:   %s%s%s\n", gris(), porque, fin());
    ++r.fallos;
}

/// Resumen final con los tres recuentos.
inline void resumen(const char *que, const Recuento &r) {
    const char *tinte = r.fallos != 0 ? rojo() : (r.pendientes != 0 ? ambar()
                                                                   : verde());
    std::printf("\n%s[%s]%s %s%d correctas%s, %s%d pendientes%s, %s%d fallos%s "
                "-> %s%s%s\n",
                fuerte(), que, fin(), verde(), r.ok, fin(), ambar(), r.pendientes,
                fin(), rojo(), r.fallos, fin(), tinte,
                r.fallos == 0 ? "OK" : "CON FALLOS", fin());
}

} // namespace tests

#endif // TESTS_UTIL_TEST_REPORT_H
