/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file test_must_not_overlap.cpp
 * @brief Que la disyuncion DEMOSTRADA no se apoye en ninguna promesa.
 *
 * Hay dos preguntas y tienen que ser dos funciones:
 *
 *     optimizacion (@c may_alias):       pueden pisarse, con lo que estamos
 *                                        dispuestos a asumir?
 *     verificacion (@c must_not_overlap): pueden pisarse, SOLO CON LA
 *                                        EVIDENCIA?
 *
 * Mientras las contesto la misma, la comprobacion de una promesa se apoyaba en
 * la propia promesa: dos parametros que declaran exclusividad se daban por
 * disjuntos, y con eso la llamada que les pasaba algo quedaba "verificada".
 *
 * Esto NO lo puede guardar la suite e2e, y por eso vive aqui: la propiedad es
 * interna y no se ve en la salida de ningun programa.  Un ejemplo del corpus
 * no distingue "no se apoya en la promesa" de "se apoya y acierta".
 *
 * Se comprueban las DOS a la vez en cada caso, que es lo unico que demuestra
 * que se separaron: si se volvieran a juntar, las lineas marcadas con
 * EL CORTE fallarian.
 */

#include "analysis/effects/effects.h"

#include <cstdio>

using namespace analysis::effects;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fail;                                                          \
            std::printf("FALLO [%s:%d]: %s\n", __FILE__, __LINE__, msg);       \
        }                                                                      \
    } while (0)

/// @brief Un sitio concreto: clase, raiz, desplazamiento y ancho.
static AbstractLoc loc(AbstractLoc::Kind k, uint32_t id, int64_t off,
                       int64_t width, bool exclusive = false) {
    AbstractLoc l;
    l.kind = k;
    l.id = id;
    l.off = off;
    l.width = width;
    l.exclusive = exclusive;
    return l;
}

int main() {
    using K = AbstractLoc::Kind;

    /* ------------------------------------------------------------------
     * EL CORTE.  Dos parametros con indices distintos que declaran los dos
     * su exclusividad.
     *
     * Para OPTIMIZAR se dan por disjuntos: el contrato lo dice, y para eso
     * esta un contrato.
     *
     * Para VERIFICAR no se demuestra nada: la raiz de un parametro es su
     * POSICIoN -- un nombre --, y nada impide que quien llama pase la misma
     * direccion dos veces.  Si esta linea falla, las dos preguntas han vuelto
     * a ser una y la comprobacion de una promesa vuelve a apoyarse en ella.
     * ------------------------------------------------------------------ */
    {
        const AbstractLoc a = loc(K::ArgDerived, 0, 0, 8, /*exclusive=*/true);
        const AbstractLoc b = loc(K::ArgDerived, 1, 0, 8, /*exclusive=*/true);
        CHECK(!may_alias(a, b),
              "optimizacion: dos parametros que declaran se dan por disjuntos");
        CHECK(!must_not_overlap(a, b),
              "EL CORTE: la declaracion NO demuestra la disyuncion");
    }

    /* Y sin declarar, las dos coinciden en lo conservador. */
    {
        const AbstractLoc a = loc(K::ArgDerived, 0, 0, 8);
        const AbstractLoc b = loc(K::ArgDerived, 1, 0, 8);
        CHECK(may_alias(a, b), "sin declarar, pueden pisarse");
        CHECK(!must_not_overlap(a, b), "y tampoco se demuestra lo contrario");
    }

    /* Una sola declarando tampoco basta para optimizar: se exigen las dos. */
    {
        const AbstractLoc a = loc(K::ArgDerived, 0, 0, 8, /*exclusive=*/true);
        const AbstractLoc b = loc(K::ArgDerived, 1, 0, 8);
        CHECK(may_alias(a, b), "una sola declaracion no basta ni para asumir");
        CHECK(!must_not_overlap(a, b), "y menos para demostrar");
    }

    /* ------------------------------------------------------------------
     * Lo que SI es evidencia, y que las dos tienen que contestar igual.
     * ------------------------------------------------------------------ */

    // Raices concretas distintas de la misma clase: dos reservas, dos objetos.
    {
        const AbstractLoc a = loc(K::Stack, 1, 0, 8);
        const AbstractLoc b = loc(K::Stack, 2, 0, 8);
        CHECK(!may_alias(a, b), "dos locales distintos no se pisan");
        CHECK(must_not_overlap(a, b), "y esta demostrado, sin promesa alguna");
    }

    // Clases distintas.
    {
        const AbstractLoc a = loc(K::Stack, 1, 0, 8);
        const AbstractLoc b = loc(K::Heap, 1, 0, 8);
        CHECK(must_not_overlap(a, b), "la pila no es el monton");
    }

    // Misma raiz, rangos que no se cortan.
    {
        const AbstractLoc a = loc(K::Stack, 1, 0, 8);
        const AbstractLoc b = loc(K::Stack, 1, 8, 8);
        CHECK(must_not_overlap(a, b), "campos contiguos y disjuntos");
        CHECK(!must_overlap(a, b), "y no se solapan");
    }

    // Misma raiz, rangos que SI se cortan: ni disjunto ni dudoso.
    {
        const AbstractLoc a = loc(K::Stack, 1, 0, 8);
        const AbstractLoc b = loc(K::Stack, 1, 4, 8);
        CHECK(!must_not_overlap(a, b), "rangos cortados: no son disjuntos");
        CHECK(must_overlap(a, b), "y el solape SI se demuestra");
    }

    /* Misma raiz con un ancho SIN CONOCER: no se demuestra ninguna de las dos.
     * Es la asimetria que hace falta -- no se puede afirmar que acabe antes de
     * donde empieza el otro --, y es donde un predicado descuidado afirmaria
     * de mas. */
    {
        const AbstractLoc a = loc(K::Stack, 1, 0, 0);
        const AbstractLoc b = loc(K::Stack, 1, 64, 8);
        CHECK(!must_not_overlap(a, b),
              "ancho desconocido: no consta disyuncion");
    }

    /* TOP alcanza cualquier cosa y la clase generica es "cualquiera de esta
     * clase": de ninguno se demuestra que no toque algo. */
    {
        const AbstractLoc top = loc(K::Unknown, 0, 0, 0);
        const AbstractLoc a = loc(K::Stack, 1, 0, 8);
        CHECK(!must_not_overlap(top, a), "TOP no demuestra disyuncion");
        CHECK(!must_not_overlap(a, top), "ni por el otro lado");
    }
    {
        const AbstractLoc gen = loc(K::Stack, LOC_GENERIC, 0, 0);
        const AbstractLoc a = loc(K::Stack, 1, 0, 8);
        CHECK(!must_not_overlap(gen, a), "la raiz generica tampoco");
    }

    /* BOTTOM no toca nada, asi que la disyuncion con cualquiera esta
     * demostrada.  Es el unico caso que se cierra mirando a uno solo. */
    {
        const AbstractLoc none = loc(K::None, 0, 0, 0);
        const AbstractLoc a = loc(K::Stack, 1, 0, 8);
        CHECK(must_not_overlap(none, a), "BOTTOM no toca nada");
    }

    /* Y la propiedad que las relaciona: NUNCA pueden ser ciertas las dos.
     * Demostrar que se tocan y demostrar que no es una contradiccion, y si
     * alguna vez sale, el fallo esta en uno de los dos y no en quien pregunta.
     */
    {
        const AbstractLoc casos[] = {
            loc(K::Stack, 1, 0, 8),
            loc(K::Stack, 1, 4, 8),
            loc(K::Stack, 2, 0, 8),
            loc(K::Heap, 1, 0, 8),
            loc(K::ArgDerived, 0, 0, 8, true),
            loc(K::ArgDerived, 1, 0, 8, true),
            loc(K::Unknown, 0, 0, 0),
            loc(K::None, 0, 0, 0),
            loc(K::Stack, LOC_GENERIC, 0, 0),
        };
        const size_t n = sizeof(casos) / sizeof(casos[0]);
        bool ok = true;
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < n; ++j)
                if (must_overlap(casos[i], casos[j]) &&
                    must_not_overlap(casos[i], casos[j]))
                    ok = false;
        CHECK(ok, "nunca se demuestran a la vez el solape y la disyuncion");
    }

    std::printf("%s: %d comprobaciones, %d fallidas\n",
                g_fail == 0 ? "OK" : "FALLO", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
