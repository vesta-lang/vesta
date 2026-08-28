/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/vx/test_vx_builtin_names.cpp
 * @brief Comprueba que reconocer un builtin por tabla da lo mismo que
 *        compararlo contra su nombre.
 *
 * El orden de la tabla y que coincida con el enum ya los verifica el
 * compilador con dos `static_assert`, asi que aqui no hace falta repetirlo.
 * Lo que el compilador NO puede comprobar es lo unico que importa de verdad:
 * que la busqueda encuentra cada nombre y devuelve el que corresponde, y que
 * NO encuentra lo que no es un builtin.
 *
 * El caso peligroso no es el nombre que no existe: es el que se parece.  Un
 * `print` con una letra de mas, un prefijo de otro builtin, un nombre con la
 * longitud de uno de la tabla pero distinto contenido -- ahi es donde un
 * comparador que mira la longitud antes que los bytes puede equivocarse --.
 */
#include "vx/builtin_names.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fallos = 0; ///< Cuantas comprobaciones han fallado.

/**
 * @brief Comprueba una condicion y deja constancia si no se cumple.
 *
 * @param cond Lo que tiene que ser cierto.
 * @param que  Que se estaba comprobando, para el mensaje.
 */
void check(bool cond, const std::string &que) {
    if (!cond) {
        std::printf("FALLO: %s\n", que.c_str());
        ++g_fallos;
    }
}

} // namespace

/**
 * @brief Punto de entrada del test.
 *
 * @return 0 si todo paso, 1 si hubo algun fallo.
 */
int main() {
    using vx::Builtin;
    using vx::builtin_from_name;
    using vx::builtin_name;

    /* Ida y vuelta sobre TODOS: el enum va de Unknown+1 a Count-1, y cada uno
     * tiene que tener texto, y ese texto tiene que devolver ese mismo valor.
     * Esto recorre la tabla entera sin escribir aqui los doscientos nombres,
     * que seria una tercera copia de la lista -- justo lo que se quito. */
    const auto primero = static_cast<uint16_t>(Builtin::Unknown) + 1;
    const auto ultimo = static_cast<uint16_t>(Builtin::Count);
    int vistos = 0;
    for (uint16_t v = static_cast<uint16_t>(primero); v < ultimo; ++v) {
        const auto b = static_cast<Builtin>(v);
        const std::string txt(builtin_name(b));
        check(!txt.empty(), "el builtin " + std::to_string(v) + " no tiene texto");
        if (txt.empty())
            continue;
        check(builtin_from_name(txt) == b, "ida y vuelta de '" + txt + "'");
        ++vistos;
    }
    check(vistos > 200, "se esperaban mas de doscientos builtins, hay " +
                            std::to_string(vistos));

    /* Un nombre que no lo es no puede colarse. */
    check(builtin_from_name("") == Builtin::Unknown, "la cadena vacia");
    check(builtin_from_name("mi_funcion") == Builtin::Unknown, "nombre normal");

    /* Y los que se PARECEN, que es donde falla un comparador mal escrito:
     * prefijos, sufijos, y nombres de la misma longitud que uno real. */
    check(builtin_from_name("prin") == Builtin::Unknown, "prefijo de print");
    check(builtin_from_name("printl") == Builtin::Unknown, "prefijo de println");
    check(builtin_from_name("printlnn") == Builtin::Unknown, "println con letra de mas");
    check(builtin_from_name("qrint") == Builtin::Unknown, "misma longitud que print");
    check(builtin_from_name("printz") == Builtin::Unknown, "misma longitud que printf");
    check(builtin_from_name("PRINT") == Builtin::Unknown, "print en mayusculas");

    /* Los extremos de la tabla: el mas corto y el mas largo se reconocen, y
     * uno mas corto o mas largo que cualquiera se descarta sin buscar. */
    check(builtin_from_name("Ok") == Builtin::Ok, "el nombre mas corto");
    check(builtin_from_name("a") == Builtin::Unknown, "mas corto que ninguno");
    check(builtin_from_name(std::string(64, 'x')) == Builtin::Unknown,
          "mas largo que ninguno");

    /* Unas cuantas a mano, de familias distintas, para que un error de
     * indexado global no pase entre el ida-y-vuelta. */
    check(builtin_from_name("print") == Builtin::Print, "print");
    check(builtin_from_name("println") == Builtin::Println, "println");
    check(builtin_from_name("malloc") == Builtin::Malloc, "malloc");
    check(builtin_from_name("term_move") == Builtin::TermMove, "term_move");
    check(builtin_from_name("shared_malloc") == Builtin::SharedMalloc, "shared_malloc");
    check(builtin_from_name("None") == Builtin::None, "None (el de Optional)");

    /* Un builtin invalido no revienta al pedir su texto. */
    check(builtin_name(Builtin::Unknown).empty(), "Unknown no tiene texto");
    check(builtin_name(Builtin::Count).empty(), "Count no tiene texto");

    if (g_fallos == 0)
        std::printf("OK: %d builtins, ida y vuelta correcta\n", vistos);
    else
        std::printf("%d comprobaciones fallidas\n", g_fallos);
    return g_fallos == 0 ? 0 : 1;
}
