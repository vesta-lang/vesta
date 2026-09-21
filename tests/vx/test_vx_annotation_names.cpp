/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/vx/test_vx_annotation_names.cpp
 * @brief Comprueba la tabla de anotaciones: que reconoce las que hay, que
 *        rechaza lo que no lo es, y que la sugerencia sirve.
 *
 * Lo que esta tabla decide es si un `@algo` compila, asi que sus dos formas de
 * fallar son caras y opuestas:
 *
 *   - Dejarse una anotacion buena fuera -> el compilador rechaza codigo
 *     correcto, y el mensaje acusa a algo que si existe.
 *   - Aceptar cualquier cosa -> se vuelve al agujero que la tabla vino a
 *     cerrar, donde una errata no hacia nada y nadie se enteraba.
 *
 * La sugerencia tiene su propio riesgo: si acepta cualquier parecido, un
 * nombre inventado de arriba abajo arrastra la entrada mas corta de la tabla y
 * despista mas que ayudar.  Por eso se comprueban LAS DOS caras -- que sugiere
 * cuando debe y que CALLA cuando no hay nada cerca --.
 */
#include "vx/annotation_names.h"

#include <cstdio>
#include <cstring>
#include <string>

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
 * @return 0 si todo paso, 1 si hubo algun fallo.
 */
int main() {
    using vx::annotation_all;
    using vx::annotation_exists;
    using vx::annotation_nearest;

    size_t n = 0;
    const char *const *todas = annotation_all(&n);
    check(n > 0, "la tabla esta vacia");

    /* Toda entrada tiene texto y se encuentra a si misma.  Recorrer la tabla
     * en vez de escribir aqui los nombres evita una SEGUNDA copia de la lista,
     * que es justo lo que se vino a quitar. */
    for (size_t i = 0; i < n; ++i) {
        const std::string name(todas[i]);
        check(!name.empty(),
              "la entrada " + std::to_string(i) + " no tiene texto");
        check(annotation_exists(name), "no se encuentra '" + name + "'");
    }

    /* El orden alfabetico no lo usa la busqueda, pero si quien lee la tabla y
     * quien la lista.  Se comprueba aqui porque nada mas lo hace: una entrada
     * metida en medio no rompe nada y se queda. */
    for (size_t i = 1; i < n; ++i)
        check(std::strcmp(todas[i - 1], todas[i]) < 0,
              std::string("la tabla no esta ordenada: '") + todas[i - 1] +
                  "' va antes que '" + todas[i] + "'");

    /* Unas cuantas que TIENEN que estar, escritas a mano: si alguien vacia la
     * tabla, los bucles de arriba pasan sin comprobar nada. */
    check(annotation_exists("Override"), "falta @Override");
    check(annotation_exists("Provides"), "falta @Provides");
    check(annotation_exists("Target"), "falta @Target");
    check(annotation_exists("Naked"), "falta @Naked");
    check(annotation_exists("Getter"), "falta @Getter (Lombok)");
    check(annotation_exists("Data"), "falta @Data (Lombok)");
    check(annotation_exists("complexity"), "falta @complexity");

    /* Las RETIRADAS no estan a proposito: tienen su propio mensaje, que dice el
     * reemplazo, y ese es mejor que "no existe". */
    check(!annotation_exists("AllocatorOverride"),
          "@AllocatorOverride esta retirada y no debe estar en la tabla");
    check(!annotation_exists("PanicHandler"),
          "@PanicHandler esta retirada y no debe estar en la tabla");

    /* Y lo que no es una anotacion, no lo es.  `as` e `impl` son palabras
     * contextuales y `VirtualPtr` es un tipo: los tres se comparan en el
     * parser, que es de donde salio la tabla, asi que son justo los que
     * podrian haberse colado. */
    check(!annotation_exists("Ovrride"), "'Ovrride' no es una anotacion");
    check(!annotation_exists(""), "la cadena vacia no es una anotacion");
    check(!annotation_exists("as"), "'as' es una palabra, no una anotacion");
    check(!annotation_exists("impl"),
          "'impl' es una palabra, no una anotacion");
    check(!annotation_exists("VirtualPtr"),
          "'VirtualPtr' es un tipo, no una anotacion");

    /* La sugerencia acierta en las eratas de una y dos letras... */
    const char *s = annotation_nearest("Ovrride");
    check(s != nullptr && std::string(s) == "Override",
          "'Ovrride' deberia sugerir '@Override'");
    s = annotation_nearest("Provdes");
    check(s != nullptr && std::string(s) == "Provides",
          "'Provdes' deberia sugerir '@Provides'");
    s = annotation_nearest("Targt");
    check(s != nullptr && std::string(s) == "Target",
          "'Targt' deberia sugerir '@Target'");

    /* ...y CALLA cuando no hay nada cerca.  Sin este tope la sugerencia
     * despista: un nombre inventado entero arrastraba cualquier entrada. */
    check(annotation_nearest("Zumbido") == nullptr,
          "'Zumbido' no se parece a ninguna: no debe sugerir nada");
    check(annotation_nearest("") == nullptr,
          "la cadena vacia no debe sugerir nada");

    if (g_fallos == 0)
        std::printf("test_vx_annotation_names: OK (%zu anotaciones)\n", n);
    return g_fallos == 0 ? 0 : 1;
}
