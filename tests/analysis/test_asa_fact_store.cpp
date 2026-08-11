/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/analysis/test_asa_fact_store.cpp
 * @brief La frontera comun del ASA: que un hecho lleve encima su certeza, su
 *        origen y su PRUEBA, que la prueba se pueda recorrer, y que producir y
 *        mirar sean cosas distintas.
 *
 * El test EXIGE las propiedades que hacen util al sistema, no que compile:
 * que un consumidor pueda preguntar POR QUE y recibir la derivacion; que dos
 * hechos del mismo texto compartan UNA copia (o el almacen crece con el
 * programa); y que un hecho observado en ejecucion se distinga de uno demostrado
 * SIN que el consumidor tenga que saber quien lo produjo.
 */

#include "analysis/asa/fact_store.h"
#include "analysis/asa/productores.h"
#include "ir/ssa_ir.h"

#include <cstdio>
#include <string>

using namespace analysis::asa;

static int g_checks = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fail;                                                          \
            std::printf("  [FALLO] %s (linea %d)\n", (msg), __LINE__);         \
        }                                                                      \
    } while (0)

/// Un hecho cualquiera, para no repetir el armado en cada prueba.
static Fact hecho(FactStore &a, const char *dominio, const char *codigo,
                  const char *funcion, Certeza c) {
    Fact f;
    f.que.dominio = dominio;
    f.que.codigo = codigo;
    f.que.detalle = a.internar("detalle repetido que no cabe en la pila");
    f.de_quien.clase = Sujeto::Clase::Funcion;
    f.de_quien.funcion = a.internar(funcion);
    f.sello.certeza = c;
    f.sello.origen.productor = dominio;
    return f;
}

// ===========================================================================
// 1. Un hecho lleva su prueba, y la prueba se recorre
// ===========================================================================
static void probar_derivacion() {
    FactStore a;
    const FactId base = a.anadir(hecho(a, "asa.estructura", "estructura.forma",
                                       "f", Certeza::Demostrada));

    Fact rango = hecho(a, "asa.rangos", "rango.acotado", "f", Certeza::Demostrada);
    rango.prueba.regla = "flujo-de-datos";
    rango.prueba.de.push_back(base);
    const FactId r = a.anadir(std::move(rango));

    Fact derivado = hecho(a, "asa.memoria", "memoria.apunta_a", "f",
                          Certeza::Demostrada);
    derivado.prueba.regla = "propagacion";
    derivado.prueba.de.push_back(r);
    const FactId d = a.anadir(std::move(derivado));

    const std::vector<FactId> cadena = a.explicar(d);
    CHECK(cadena.size() == 3, "la derivacion llega hasta el hecho fundacional");
    CHECK(cadena[0] == d && cadena[1] == r && cadena[2] == base,
          "y viene en orden: primero el hecho, detras aquello de lo que se sigue");

    /* Un grafo, no un arbol: dos hechos pueden apoyarse en el mismo y la
     * explicacion no debe repetirlo. */
    Fact otro = hecho(a, "asa.bucles", "bucle.cabecera", "f", Certeza::Demostrada);
    otro.prueba.de.push_back(base);
    otro.prueba.de.push_back(r);
    const FactId o = a.anadir(std::move(otro));
    const std::vector<FactId> c2 = a.explicar(o);
    CHECK(c2.size() == 3, "un apoyo compartido no se cuenta dos veces");
}

// ===========================================================================
// 2. Los textos se comparten: un almacen no crece con lo que se repite
// ===========================================================================
static void probar_internado() {
    FactStore a;
    const char *x = a.internar("mismo texto");
    const char *y = a.internar("mismo texto");
    CHECK(x == y, "dos textos iguales son UNA copia, no dos");
    CHECK(std::string(x) == "mismo texto", "y el texto es el que se guardo");
    CHECK(a.internar("otro") != x, "textos distintos no se confunden");
}

// ===========================================================================
// 3. Se consulta por sujeto y por dominio sin recorrerlo todo
// ===========================================================================
static void probar_consulta() {
    FactStore a;
    a.anadir(hecho(a, "asa.rangos", "rango.acotado", "uno", Certeza::Demostrada));
    a.anadir(hecho(a, "asa.rangos", "rango.acotado", "dos", Certeza::Demostrada));
    a.anadir(hecho(a, "asa.memoria", "memoria.apunta_a", "uno",
                   Certeza::Demostrada));

    CHECK(a.de_funcion("uno").size() == 2, "lo que se sabe de una funcion");
    CHECK(a.de_funcion("dos").size() == 1, "y de otra, sin mezclarse");
    CHECK(a.de_funcion("ninguna").empty(),
          "preguntar por algo que no esta no inventa nada");
    CHECK(a.de_dominio("asa.rangos").size() == 2, "lo que dijo un dominio");
}

// ===========================================================================
// 4. La certeza distingue al hecho observado del demostrado -- y el consumidor
//    NO necesita saber quien lo produjo
// ===========================================================================
static void probar_certeza_agnostica() {
    FactStore a;
    /* El mismo hecho, dicho por dos fuentes distintas.  El consumidor mira la
     * certeza: sobre lo demostrado puede quitar la comprobacion; sobre lo
     * observado tiene que dejar red (una guarda). */
    a.anadir(hecho(a, "asa.rangos", "rango.acotado", "f", Certeza::Demostrada));
    a.anadir(hecho(a, "asa.observado", "rango.acotado", "f", Certeza::Inferida));

    const FactStore::Recuento c = a.recuento();
    CHECK(c.demostradas == 1 && c.inferidas == 1,
          "los dos conviven: nada se sobreescribe");

    int con_red = 0, sin_red = 0;
    for (FactId id : a.de_funcion("f")) {
        /* Asi decide un consumidor: por la certeza, sin mirar la procedencia. */
        if (a.at(id).sello.certeza == Certeza::Demostrada) ++sin_red;
        else ++con_red;
    }
    CHECK(sin_red == 1 && con_red == 1,
          "la decision sale de la certeza, no de quien lo dijo");
}

// ===========================================================================
// 5. Producir y mirar son cosas distintas: los dominios se dan de alta
// ===========================================================================
static void probar_registro() {
    const std::vector<const char *> ds = productores_registrados();
    CHECK(ds.size() >= 5,
          "los dominios de casa estan dados de alta (estructura, rangos, "
          "frontera, memoria, bucles)");
    bool estructura_primero = ds.empty() ? false
                                         : std::string(ds[0]) == "asa.estructura";
    CHECK(estructura_primero,
          "la estructura va primero: los demas apoyan sus hechos en el suyo");
}

int main() {
    std::printf("=== test_asa_fact_store (la frontera comun del ASA) ===\n");
    probar_derivacion();
    probar_internado();
    probar_consulta();
    probar_certeza_agnostica();
    probar_registro();
    std::printf("%s: %d comprobaciones, %d fallos\n",
                g_fail == 0 ? "OK" : "FALLO", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
