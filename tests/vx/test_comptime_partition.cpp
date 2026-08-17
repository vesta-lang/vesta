/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file test_comptime_partition.cpp
 * @brief Que entra y que NO entra en el conjunto comptime de un modulo
 *        (@ref vx::collect_comptime_unit), y que su @c content_hash sirve de
 *        clave de cache.
 *
 * Ese conjunto decide que se compilara APARTE del codigo normal por ejecutarse
 * al compilar.  Hoy el artefacto que alimenta a la ComptimeVM es MONOLITICO --
 * se construye compilando el proyecto entero -- y eso cuesta el ~43% de una
 * compilacion en frio, con la agravante de que tocar UNA sola funcion comptime
 * lo regenera ENTERO.  El recolector es la base para cachearlo por unidades, y
 * hasta ahora no tenia ninguna prueba.
 *
 * Tres errores lo romperian sin que nada mas los detecte:
 *
 *  - **Dejar fuera un `@Macro`**.  No es una `comptime fn` con otro nombre:
 *    tiene su propia marca y su propio camino de invocacion.  Un recolector que
 *    solo mirase la palabra `comptime` construiria el artefacto sin los macros y
 *    rompeeria esa feature entera sin que ninguna prueba del `inject` fallara.
 *  - **Perder una dependencia**.  Si el codigo comptime llama a una funcion
 *    normal y esa no viaja en el artefacto, el artefacto no es auto-suficiente y
 *    la llamada revienta EN COMPILACION, que es el peor momento.
 *  - **Un `content_hash` que reacciona a lo que no debe**.  Si cambiar codigo
 *    NO-comptime altera el hash, el cache falla siempre y la separacion no
 *    ahorra nada; si NO cambia al tocar una comptime, se reusa un artefacto
 *    obsoleto, que es peor.
 */

#include "vx/comptime/comptime_collect.h"
#include "vx/lexer.h"
#include "vx/parser.h"

#include <cstdio>
#include <string>
#include <vector>

using vx::ComptimeUnit;
using vx::Diagnostics;
using vx::Lexer;
using vx::Parser;

static int g_pass = 0;
static int g_fail = 0;

#define CK(cond)                                                               \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_pass;                                                          \
        } else {                                                               \
            ++g_fail;                                                          \
            std::printf("FAIL linea %d: %s\n", __LINE__, #cond);                \
        }                                                                      \
    } while (0)

/// Parsea @p src y devuelve su conjunto comptime.  @p ok queda a false si el
/// fuente no parsea (asi un fallo de sintaxis del test no se lee como un fallo
/// del recolector).
static ComptimeUnit unidad_de(const std::string &src, bool &ok) {
    Diagnostics diags;
    Lexer lx(src, "<test>", diags);
    Parser p(lx, diags);
    auto mod = p.parse_program();
    ok = (mod != nullptr) && !diags.has_errors();
    if (!ok) return {};
    return vx::collect_comptime_unit(*mod, src);
}

/// ¿Esta @p name en @p v?
static bool tiene(const std::vector<std::string> &v, const std::string &name) {
    for (const std::string &s : v)
        if (s == name) return true;
    return false;
}

/// Imprime una lista cuando una afirmacion falla: un "no coincide" a secas
/// obliga a reejecutar a mano para ver que sobra o que falta.
static void volcar(const char *etiqueta, const std::vector<std::string> &v) {
    std::printf("  %s (%zu):", etiqueta, v.size());
    for (const std::string &s : v) std::printf(" %s", s.c_str());
    std::printf("\n");
}

int main() {
    /* CASO 1 -- `comptime` entra, `@Macro` entra Y VAN SEPARADOS, lo normal no.
     * Que vayan en listas distintas importa: se invocan por caminos distintos. */
    {
        const std::string src = R"VX(
comptime string gen_texto(i64 n) { return "x"; }
@Macro
comptime string mi_macro(i64 n) { return "y"; }
i64 funcion_normal(i64 a) { return a + 1; }
i32 main() { return 0; }
)VX";
        bool ok = false;
        const ComptimeUnit u = unidad_de(src, ok);
        CK(ok);
        if (ok) {
            const bool bien = tiene(u.comptime_fns, "gen_texto") &&
                              tiene(u.macros, "mi_macro") &&
                              !tiene(u.comptime_fns, "mi_macro") &&
                              !tiene(u.comptime_fns, "funcion_normal") &&
                              !tiene(u.macros, "funcion_normal") &&
                              !tiene(u.helper_deps, "funcion_normal");
            CK(bien);
            if (!bien) {
                std::printf("FAIL [basico]:\n");
                volcar("comptime_fns", u.comptime_fns);
                volcar("macros", u.macros);
                volcar("helper_deps", u.helper_deps);
            }
            CK(!u.empty());
        }
    }

    /* CASO 2 -- un modulo SIN nada comptime da un conjunto VACIO.  Si diera
     * algo, cada modulo arrastraria artefacto comptime que no necesita, que es
     * justo el coste que la separacion existe para eliminar. */
    {
        const std::string src = R"VX(
i64 suma(i64 a, i64 b) { return a + b; }
struct Punto { f64 x; f64 y; }
i32 main() { return 0; }
)VX";
        bool ok = false;
        const ComptimeUnit u = unidad_de(src, ok);
        CK(ok);
        if (ok) {
            CK(u.empty());
            CK(u.content_hash == 0);
        }
    }

    /* CASO 3 -- LA DEPENDENCIA VIAJA.  Una comptime que llama a una funcion
     * normal la arrastra: sin eso el artefacto no es auto-suficiente y la
     * llamada falla al compilar. */
    {
        const std::string src = R"VX(
i64 ayudante(i64 n) { return n * 3; }
comptime i64 usa_ayudante(i64 n) { return ayudante(n) + 1; }
i64 nadie_la_llama(i64 n) { return n; }
i32 main() { return 0; }
)VX";
        bool ok = false;
        const ComptimeUnit u = unidad_de(src, ok);
        CK(ok);
        if (ok) {
            const bool bien = tiene(u.comptime_fns, "usa_ayudante") &&
                              tiene(u.helper_deps, "ayudante") &&
                              !tiene(u.helper_deps, "nadie_la_llama");
            CK(bien);
            if (!bien) {
                std::printf("FAIL [deps]:\n");
                volcar("comptime_fns", u.comptime_fns);
                volcar("helper_deps", u.helper_deps);
            }
        }
    }

    /* CASO 4 -- EL HASH REACCIONA A LO QUE DEBE, Y SOLO A ESO.  Es la clave de
     * cache del artefacto separado; de esto depende que la separacion ahorre
     * algo.  Tocar codigo NO-comptime NO puede moverlo; tocar una comptime SI. */
    {
        const std::string base = R"VX(
comptime i64 doble(i64 n) { return n * 2; }
i64 no_comptime(i64 a) { return a + 1; }
i32 main() { return 0; }
)VX";
        // Igual, pero cambiando SOLO codigo no-comptime.
        const std::string cambia_normal = R"VX(
comptime i64 doble(i64 n) { return n * 2; }
i64 no_comptime(i64 a) { return a + 99999; }
i32 main() { return 0; }
)VX";
        // Igual, pero cambiando el cuerpo de la COMPTIME.
        const std::string cambia_comptime = R"VX(
comptime i64 doble(i64 n) { return n * 3; }
i64 no_comptime(i64 a) { return a + 1; }
i32 main() { return 0; }
)VX";
        bool o1 = false, o2 = false, o3 = false;
        const ComptimeUnit ub = unidad_de(base, o1);
        const ComptimeUnit un = unidad_de(cambia_normal, o2);
        const ComptimeUnit uc = unidad_de(cambia_comptime, o3);
        CK(o1 && o2 && o3);
        if (o1 && o2 && o3) {
            CK(ub.content_hash != 0);
            const bool estable = ub.content_hash == un.content_hash;
            CK(estable);
            if (!estable)
                std::printf("FAIL [hash]: cambiar codigo NO-comptime movio el "
                            "hash (%llu -> %llu): el cache fallaria siempre\n",
                            (unsigned long long)ub.content_hash,
                            (unsigned long long)un.content_hash);
            const bool reacciona = ub.content_hash != uc.content_hash;
            CK(reacciona);
            if (!reacciona)
                std::printf("FAIL [hash]: cambiar una COMPTIME no movio el hash "
                            "(%llu): se reusaria un artefacto obsoleto\n",
                            (unsigned long long)ub.content_hash);
        }
    }

    std::printf("\n=== test_comptime_partition: %d OK, %d fallidos ===\n",
                g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
