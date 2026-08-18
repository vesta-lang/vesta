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
 *    solo mirase la palabra `comptime` construiria el artefacto sin los macros
 * y romperia esa feature entera sin que ninguna prueba del `inject` fallara.
 *  - **Perder una dependencia**.  Si el codigo comptime llama a una funcion
 *    normal y esa no viaja en el artefacto, el artefacto no es auto-suficiente
 * y la llamada revienta EN COMPILACION, que es el peor momento.
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
            std::printf("FAIL linea %d: %s\n", __LINE__, #cond);               \
        }                                                                      \
    } while (0)

/**
 * @brief Parsea @p src y devuelve su conjunto comptime.
 *
 * @param src Fuente Vesta del caso.
 * @param ok  [salida] @c false si el fuente no parsea.  Se separa del resultado
 *            para que un fallo de sintaxis DEL TEST no se lea como un fallo del
 *            recolector, que son cosas distintas.
 * @return El conjunto comptime, o uno vacio si @p ok quedo a @c false.
 */
static ComptimeUnit unidad_de(const std::string &src, bool &ok) {
    Diagnostics diags;
    Lexer lx(src, "<test>", diags);
    Parser p(lx, diags);
    auto mod = p.parse_program();
    ok = (mod != nullptr) && !diags.has_errors();
    if (!ok) return {};
    return vx::collect_comptime_unit(*mod, src);
}

/**
 * @brief Pertenencia de @p name a @p v.
 *
 * @param v    Lista donde buscar.
 * @param name Nombre buscado.
 * @return @c true si @p v contiene @p name.
 */
static bool tiene(const std::vector<std::string> &v, const std::string &name) {
    for (const std::string &s : v)
        if (s == name) return true;
    return false;
}

/**
 * @brief Imprime una lista del conjunto cuando una afirmacion falla.
 *
 * Un "no coincide" a secas obliga a reejecutar a mano para ver que sobra o que
 * falta; con la lista delante el fallo se lee de una vez.
 *
 * @param etiqueta Nombre de la lista (@c comptime_fns, @c macros, ...).
 * @param v        Contenido a volcar.
 */
static void volcar(const char *etiqueta, const std::vector<std::string> &v) {
    std::printf("  %s (%zu):", etiqueta, v.size());
    for (const std::string &s : v)
        std::printf(" %s", s.c_str());
    std::printf("\n");
}

int main() {
    /* CASO 1 -- `comptime` entra, `@Macro` entra Y VAN SEPARADOS, lo normal no.
     * Que vayan en listas distintas importa: se invocan por caminos distintos.
     */
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
     * algo.  Tocar codigo NO-comptime NO puede moverlo; tocar una comptime SI.
     */
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
                std::printf(
                    "FAIL [hash]: cambiar una COMPTIME no movio el hash "
                    "(%llu): se reusaria un artefacto obsoleto\n",
                    (unsigned long long)ub.content_hash);
        }
    }

    /* CASO 5 -- EL TEXTO EXTRAIDO es lo que se compilara aparte, asi que tiene
     * que llevar lo comptime y sus dependencias, los `import` (sin ellos no
     * compila por si solo) y NADA del codigo normal: cada linea de mas es
     * artefacto que se recompila en cada cambio. */
    {
        const std::string src = R"VX(
import std.io;
i64 ayudante(i64 n) { return n * 3; }
comptime i64 usa_ayudante(i64 n) { return ayudante(n) + 1; }
i64 solo_runtime(i64 n) { return n + 12345; }
i32 main() { return 0; }
)VX";
        bool ok = false;
        const ComptimeUnit u = unidad_de(src, ok);
        CK(ok);
        if (ok) {
            const bool lleva_comptime =
                u.unit_source.find("usa_ayudante") != std::string::npos;
            const bool lleva_dep =
                u.unit_source.find("ayudante(i64 n)") != std::string::npos;
            const bool lleva_import =
                u.unit_source.find("import std.io") != std::string::npos;
            const bool sin_runtime =
                u.unit_source.find("solo_runtime") == std::string::npos &&
                u.unit_source.find("12345") == std::string::npos;
            CK(lleva_comptime);
            CK(lleva_dep);
            CK(lleva_import);
            CK(sin_runtime);
            if (!(lleva_comptime && lleva_dep && lleva_import && sin_runtime)) {
                std::printf(
                    "FAIL [texto]: el fuente extraido no es el esperado."
                    "  comptime=%d dep=%d import=%d sin_runtime=%d\n",
                    (int)lleva_comptime, (int)lleva_dep, (int)lleva_import,
                    (int)sin_runtime);
                std::printf("--- extraido ---\n%s\n---\n",
                            u.unit_source.c_str());
            }
        }
    }

    /* CASO 6 -- un `import` que nadie comptime usa NO puede mover el hash: si
     * lo moviera, tocar los imports invalidaria el artefacto comptime sin que
     * nada comptime hubiera cambiado, y el cache dejaria de servir. */
    {
        const std::string sin_import = R"VX(
comptime i64 doble(i64 n) { return n * 2; }
i32 main() { return 0; }
)VX";
        const std::string con_import = R"VX(
import std.io;
comptime i64 doble(i64 n) { return n * 2; }
i32 main() { return 0; }
)VX";
        bool o1 = false, o2 = false;
        const ComptimeUnit a = unidad_de(sin_import, o1);
        const ComptimeUnit b = unidad_de(con_import, o2);
        CK(o1 && o2);
        if (o1 && o2) {
            const bool estable = a.content_hash == b.content_hash;
            CK(estable);
            if (!estable)
                std::printf(
                    "FAIL [hash-import]: anadir un import movio el hash "
                    "(0x%llx -> 0x%llx)\n",
                    (unsigned long long)a.content_hash,
                    (unsigned long long)b.content_hash);
        }
    }

    std::printf("\n=== test_comptime_partition: %d OK, %d fallidos ===\n",
                g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
