/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/vx/test_namespace_names.cpp
 * @brief Comprueba la forma fisica de un nombre con ruta de namespace.
 *
 * Esta regla estaba escrita a mano en seis sitios, y lo que se juega en ella
 * es que un simbolo se BUSQUE con el mismo nombre con que se EMITIO.  Por eso
 * se comprueban las tres piezas, y la que mas facil se rompe -- quitar el
 * prefijo -- con los casos que la rompian: un nombre que solo EMPIEZA igual
 * que la ruta, o que es la ruta sin miembro detras.
 */
#include "vx/module/namespace_names.h"
#include "vx/project/module_names.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0; ///< Cuantas comprobaciones han fallado.

/**
 * @brief Compara dos cadenas y deja constancia si no coinciden.
 * @param got  Lo obtenido.
 * @param want Lo esperado.
 * @param what Que se estaba comprobando.
 */
void check_eq(const std::string &got, const std::string &want,
              const char *what) {
    if (got != want) {
        std::printf("FALLO: %s: '%s' (esperado '%s')\n", what, got.c_str(),
                    want.c_str());
        ++g_failures;
    }
}

} // namespace

/**
 * @brief Punto de entrada del test.
 * @return 0 si todo paso, 1 si hubo algun fallo.
 */
int main() {
    using vx::namespace_member_symbol;
    using vx::namespace_symbol_path;
    using vx::qualified_symbol;
    using vx::strip_symbol_path;

    // La ruta: los puntos pasan a separador; un segmento solo, tal cual.
    check_eq(namespace_symbol_path("std.collections"), "std__collections",
             "ruta de dos segmentos");
    check_eq(namespace_symbol_path("a.b.c"), "a__b__c", "ruta de tres");
    check_eq(namespace_symbol_path("ui"), "ui", "un solo segmento");
    check_eq(namespace_symbol_path(""), "", "ruta vacia");

    // La union y el atajo con puntos dan lo mismo.
    check_eq(qualified_symbol("std__types", "uintptr"), "std__types__uintptr",
             "union con miembro");
    check_eq(namespace_member_symbol("std.types", "uintptr"),
             "std__types__uintptr", "miembro de ruta con puntos");

    // Un modulo es una ruta de un segmento: el mismo nombre por los dos
    // duenos, que es lo que permite que se crucen sin traducir.
    check_eq(vx::module_member_symbol("lib", "sumar"),
             qualified_symbol("lib", "sumar"), "modulo == ruta de un segmento");

    // Quitar el prefijo: solo si es el prefijo ENTERO seguido del separador y
    // queda algo detras.
    check_eq(strip_symbol_path("std__types__uintptr", "std__types"), "uintptr",
             "quitar la ruta");
    check_eq(strip_symbol_path("main", "app"), "main",
             "sin prefijo sale tal cual");
    check_eq(strip_symbol_path("std__typesx__f", "std__types"),
             "std__typesx__f", "empieza igual pero es OTRA ruta");
    check_eq(strip_symbol_path("std__types__", "std__types"), "std__types__",
             "ruta sin miembro detras");
    check_eq(strip_symbol_path("x", ""), "x", "ruta vacia");

    if (g_failures == 0) std::printf("test_namespace_names: OK\n");
    return g_failures == 0 ? 0 : 1;
}
