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

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

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
    igual(base,
          "i64 f(i64 x) {\n        i64 a = x * 3 + 1;\n"
          "        return a;\n}\n",
          "otra sangria");
    igual(base, base + "\n\n\n", "lineas en blanco al final");
    igual(base, "i64 f(i64 x){i64 a=x*3+1;return a;}\n", "todo en una linea");
    igual(base,
          "i64 f(i64 x) {\n    i64 a = x * 3 /* en medio */ + 1;\n"
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

    // -- Por TRAMOS ----------------------------------------------------------
    std::cout << "== hash_tokens_by_span ==\n";
    {
        /* Dos funciones seguidas, y el tramo de cada una.  Es la forma que usa
         * el indice semantico: los tramos van seguidos y cubren el fichero. */
        const std::string dos = "i64 f(i64 x) { return x + 1; }\n"
                                "i64 g(i64 y) { return y * 2; }\n";
        const uint32_t corte = 31; // donde empieza `g`
        const std::vector<vx::SourceSpan> tramos = {
            {0, corte},
            {corte, static_cast<uint32_t>(dos.size()) - corte}};

        const vx::TokenHashes h = vx::hash_tokens_by_span(dos, false, tramos);

        /* La propiedad que ATA las dos funciones: si la del fichero entero no
         * saliera de la misma cuenta, dos huellas del mismo texto podrian
         * diferir y nadie se enteraria hasta que una cache sirviera lo que no
         * era. */
        comprobar(h.whole == vx::hash_de_tokens(dos, false),
                  "la del fichero entero coincide con hash_de_tokens");
        comprobar(h.spans.size() == 2, "una huella por tramo");
        comprobar(h.spans[0] != h.spans[1],
                  "dos tramos distintos dan huellas distintas");

        /* Y lo que motivo el arreglo: tocar el FORMATO de una funcion no mueve
         * su huella, y tocar la de al lado no mueve la suya. */
        const std::string comentada = "i64 f(i64 x) { /* nota */ return x + 1; }\n"
                                      "i64 g(i64 y) { return y * 2; }\n";
        const uint32_t corte2 = 42;
        const std::vector<vx::SourceSpan> tramos2 = {
            {0, corte2},
            {corte2, static_cast<uint32_t>(comentada.size()) - corte2}};
        const vx::TokenHashes h2 =
            vx::hash_tokens_by_span(comentada, false, tramos2);

        comprobar(h2.spans[0] == h.spans[0],
                  "un comentario DENTRO no cambia la huella del tramo");
        comprobar(h2.spans[1] == h.spans[1],
                  "y no mueve la del tramo de al lado");

        /* Un tramo sin tokens da lo mismo que un fuente VACIO: una respuesta
         * que se puede comparar, no un valor que no produce nadie mas. */
        const std::vector<vx::SourceSpan> vacio = {{0, 0}};
        const vx::TokenHashes h3 = vx::hash_tokens_by_span(dos, false, vacio);
        comprobar(h3.spans[0] == vx::hash_de_tokens("", false),
                  "un tramo vacio da la huella de un fuente vacio");

        // Sin tramos: solo la del fichero, y sigue siendo la misma.
        const vx::TokenHashes h4 = vx::hash_tokens_by_span(dos, false, {});
        comprobar(h4.whole == h.whole && h4.spans.empty(),
                  "sin tramos, la del fichero no cambia");
    }

    std::cout << (fallos == 0 ? "TODO OK\n" : "HAY FALLOS\n");
    return fallos == 0 ? 0 : 1;
}
