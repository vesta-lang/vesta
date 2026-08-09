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
 * @file test_source_hash.cpp
 * @brief Comprueba que la huella de un fuente es la de lo que DICE.
 *
 * De esta huella depende que se reutilice o se tire lo ya compilado de un
 * modulo, asi que tiene dos formas de fallar y las dos importan: si cambia
 * cuando no debe, se recompila de balde; si NO cambia cuando debe, se sirve un
 * artefacto que no corresponde al fuente -- que es mucho peor.
 */

#include "vx/source_hash.h"

#include <iostream>
#include <string>

static int fallos = 0;

/// @brief Comprueba una condicion y deja constancia del caso.
static void comprobar(bool ok, const std::string &caso) {
    if (ok) {
        std::cout << "  ok   " << caso << "\n";
    } else {
        std::cout << "  FALLA " << caso << "\n";
        ++fallos;
    }
}

/// @brief Las dos fuentes dan la MISMA huella (sin contar lineas).
static void igual(const std::string &a, const std::string &b,
                  const std::string &caso) {
    comprobar(vx::hash_de_tokens(a, false) == vx::hash_de_tokens(b, false),
              caso);
}

/// @brief Las dos fuentes dan huellas DISTINTAS (sin contar lineas).
static void distinto(const std::string &a, const std::string &b,
                     const std::string &caso) {
    comprobar(vx::hash_de_tokens(a, false) != vx::hash_de_tokens(b, false),
              caso);
}

int main() {
    std::cout << "== hash_de_tokens ==\n";

    const std::string base = "i64 f(i64 x) {\n    i64 a = x * 3 + 1;\n"
                             "    return a;\n}\n";

    // -- Lo que NO cambia lo que el modulo dice ------------------------------
    igual(base, base, "el mismo texto");
    igual(base, base + "// un comentario al final\n", "comentario al final");
    igual(base, "// arriba\n" + base, "comentario al principio");
    igual(base, "i64 f(i64 x) {\n        i64 a = x * 3 + 1;\n"
                "        return a;\n}\n",
          "otra sangria");
    igual(base, base + "\n\n\n", "lineas en blanco al final");
    igual(base, "i64 f(i64 x){i64 a=x*3+1;return a;}\n",
          "todo en una linea");
    igual(base, "i64 f(i64 x) {\n    i64 a = x * 3 /* en medio */ + 1;\n"
                "    return a;\n}\n",
          "comentario de bloque en medio");

    // -- Lo que SI lo cambia -------------------------------------------------
    distinto(base, "i64 f(i64 x) {\n    i64 a = x * 4 + 1;\n    return a;\n}\n",
             "cambia una constante");
    distinto(base, "i64 f(i64 y) {\n    i64 a = y * 3 + 1;\n    return a;\n}\n",
             "cambia el nombre de un parametro");
    distinto(base, "i64 g(i64 x) {\n    i64 a = x * 3 + 1;\n    return a;\n}\n",
             "cambia el nombre de la funcion");
    distinto(base, base + "public i64 extra(i64 x) { return x; }\n",
             "aparece una funcion nueva");
    distinto("i64 a;\n", "i64 ab;\n", "dos identificadores que empiezan igual");
    distinto("f(a, b);\n", "f(ab);\n",
             "los mismos caracteres, distinta tokenizacion");
    distinto("i64 x = 1;\n", "i64 x = 1.0;\n", "entero contra flotante");
    distinto("string s = \"hola\";\n", "string s = \"adios\";\n",
             "cambia el texto de un literal");

    // -- Con las lineas dentro ----------------------------------------------
    {
        const std::string desplazado = "\n" + base;
        comprobar(vx::hash_de_tokens(base, true) !=
                      vx::hash_de_tokens(desplazado, true),
                  "con lineas: desplazar el fuente SI cambia la huella");
        comprobar(vx::hash_de_tokens(base, false) ==
                      vx::hash_de_tokens(desplazado, false),
                  "sin lineas: desplazarlo no la cambia");
        comprobar(vx::hash_de_tokens(base, true) ==
                      vx::hash_de_tokens(base + "// al final\n", true),
                  "con lineas: lo que va DESPUES no desplaza nada");
    }

    // -- Un fuente vacio ------------------------------------------------------
    comprobar(vx::hash_de_tokens("", false) ==
                  vx::hash_de_tokens("// solo un comentario\n", false),
              "vacio y solo-comentarios son lo mismo");

    std::cout << (fallos == 0 ? "TODO OK\n" : "HAY FALLOS\n");
    return fallos == 0 ? 0 : 1;
}
