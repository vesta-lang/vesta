/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file test_comptime_artifact.cpp
 * @brief El conjunto comptime de un modulo COMPILA POR SI SOLO.
 *
 * Es el eslabon que convierte el conjunto recolectado
 * (@ref vx::collect_comptime_unit) en un artefacto separado: si el texto de la
 * unidad no compila de forma aislada, no hay nada que cachear por unidad y el
 * artefacto tiene que seguir siendo el monolito de hoy -- 704 KB, 182 macros y
 * ~800 ms, el 43% de una compilacion en frio, que se rehacen enteros al tocar
 * UNA sola funcion comptime.
 *
 * "Auto-suficiente" es una promesa que el recolector hace (arrastra las
 * dependencias no-comptime y los `import`) y que hasta ahora nadie comprobaba.
 * Aqui se comprueba de la unica forma que vale: compilandolo de verdad, con el
 * mismo camino canonico que usa el compilador (@ref vesta::tc::compile), no con
 * una inspeccion del texto.
 *
 * El artefacto se escribe bajo el directorio temporal del sistema y se borra al
 * terminar: un test que deja basura en el arbol acaba ignorandose.
 */

#include "toolchain/toolchain.h"
#include "vx/comptime/comptime_artifact.h"
#include "vx/comptime/comptime_collect.h"
#include "vx/lexer.h"
#include "vx/parser.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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
 * @brief Recolecta el conjunto comptime de @p src.
 *
 * @param src Fuente Vesta del caso.
 * @param ok  [salida] @c false si el fuente no parsea.
 * @return El conjunto; vacio si @p ok quedo a @c false.
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
 * @brief Compila un fuente EN MEMORIA a un @c .velb y devuelve sus bytes.
 *
 * Usa @c source_overlay para no escribir el fuente a disco -- el texto ya esta
 * aqui --, igual que hace el LSP.
 *
 * @param fuente  Texto Vesta a compilar.
 * @param prefijo Prefijo de salida; el artefacto sera @c <prefijo>.velb.
 * @param resp    [salida] Respuesta del compilador (diagnosticos incluidos).
 * @return Bytes del @c .velb, o vacio si la compilacion fallo.
 */
static std::vector<uint8_t> compilar(const std::string &fuente,
                                     const std::string &prefijo,
                                     vesta::tc::CompileResponse &resp) {
    vesta::tc::CompileRequest req;
    req.input = prefijo + ".vx"; // nombre logico: no se abre, solo se cita
    req.source_overlay = fuente;
    req.output = prefijo;
    req.is_project = false;
    req.quiet = true;
    resp = vesta::tc::compile(req);
    if (!resp.ok) return {};
    std::ifstream f(resp.output_path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

/**
 * @brief Imprime los diagnosticos de una compilacion fallida.
 *
 * Sin esto un "no compilo" obliga a reproducirlo a mano para saber por que.
 *
 * @param resp Respuesta del compilador.
 */
static void volcar_diags(const vesta::tc::CompileResponse &resp) {
    if (!resp.message.empty())
        std::printf("  mensaje: %s\n", resp.message.c_str());
    for (const vesta::tc::Diag &d : resp.diagnostics)
        std::printf("  %s\n", d.message.c_str());
}

int main() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "vesta_test_comptime_artifact";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    /* CASO 1 -- la unidad compila SOLA, y de verdad: con su dependencia
     * no-comptime arrastrada y sin nada del codigo de runtime.
     *
     * El fuente completo tiene una funcion normal que el codigo comptime NO usa
     * y que, ademas, llama a algo inexistente: si el extractor se llevara de
     * mas, la compilacion de la unidad fallaria y el test lo diria. */
    {
        const std::string src = R"VX(
i64 ayudante(i64 n) { return n * 3; }
comptime i64 usa_ayudante(i64 n) { return ayudante(n) + 1; }
i64 solo_runtime(i64 n) { return no_existe_en_la_unidad(n); }
i32 main() { return 0; }
)VX";
        bool ok = false;
        const ComptimeUnit u = unidad_de(src, ok);
        CK(ok);
        CK(!u.unit_source.empty());
        if (ok && !u.unit_source.empty()) {
            vesta::tc::CompileResponse resp;
            const auto velb =
                compilar(u.unit_source, (dir / "unidad").string(), resp);
            CK(resp.ok);
            if (!resp.ok) {
                std::printf(
                    "FAIL [unidad]: el conjunto comptime NO compila por "
                    "si solo -- no es auto-suficiente\n");
                std::printf("--- fuente extraido ---\n%s\n---\n",
                            u.unit_source.c_str());
                volcar_diags(resp);
            }
            CK(!velb.empty());
        }
    }

    /* CASO 2 -- el fuente COMPLETO del mismo modulo NO compila (por la llamada
     * inexistente de `solo_runtime`).  Es el contraste que da valor al caso 1:
     * demuestra que la unidad compilo porque se extrajo BIEN, y no porque el
     * fuente entero compilase de todas formas. */
    {
        const std::string src = R"VX(
i64 ayudante(i64 n) { return n * 3; }
comptime i64 usa_ayudante(i64 n) { return ayudante(n) + 1; }
i64 solo_runtime(i64 n) { return no_existe_en_la_unidad(n); }
i32 main() { return 0; }
)VX";
        vesta::tc::CompileResponse resp;
        const auto velb = compilar(src, (dir / "completo").string(), resp);
        CK(!resp.ok);
        if (resp.ok)
            std::printf(
                "FAIL [contraste]: el fuente completo compilo, asi que "
                "el caso 1 no demuestra que la extraccion sea correcta; "
                "hay que endurecer el caso\n");
        (void)velb;
    }

    /* CASO 3 -- un modulo sin nada comptime no da unidad, y por tanto no hay
     * artefacto que compilar ni que cachear. */
    {
        const std::string src = R"VX(
i64 suma(i64 a, i64 b) { return a + b; }
i32 main() { return 0; }
)VX";
        bool ok = false;
        const ComptimeUnit u = unidad_de(src, ok);
        CK(ok);
        if (ok) {
            CK(u.empty());
            CK(u.unit_source.empty());
        }
    }

    /* CASO 4 -- EL AHORRO: la segunda vez NO se recompila.
     *
     * Es la razon de ser de todo esto.  Y no basta con que "funcione": hay que
     * comprobar las tres direcciones, porque cada una falla distinto.
     *   (a) la primera vez compila (no puede haber acierto de la nada);
     *   (b) la segunda REUSA -- si no, el cache no sirve y no se ahorra nada;
     *   (c) tocar la comptime NO reusa -- si reusara, se estaria ejecutando
     *       codigo comptime VIEJO, que es peor que recompilar de mas. */
    {
        const std::string src = R"VX(
comptime i64 doble(i64 n) { return n * 2; }
i32 main() { return 0; }
)VX";
        const std::string src_cambiada = R"VX(
comptime i64 doble(i64 n) { return n * 3; }
i32 main() { return 0; }
)VX";
        bool ok = false, ok2 = false;
        const ComptimeUnit u = unidad_de(src, ok);
        const ComptimeUnit u2 = unidad_de(src_cambiada, ok2);
        CK(ok && ok2);
        if (ok && ok2) {
            // Store PROPIO del test: usar el global mezclaria este caso con lo
            // que haya dejado cualquier compilacion previa de la maquina.
            const vx::CasStore cas((dir / "cas").string());
            const std::string work = (dir / "work").string();

            const vx::ComptimeArtifact a1 =
                vx::comptime_artifact_get(u, cas, work);
            CK(a1.ok);
            if (!a1.ok) std::printf("  error: %s\n", a1.error.c_str());
            CK(!a1.from_cache); // (a) la primera compila

            const vx::ComptimeArtifact a2 =
                vx::comptime_artifact_get(u, cas, work);
            CK(a2.ok);
            CK(a2.from_cache); // (b) la segunda reusa
            if (a2.ok && !a2.from_cache)
                std::printf("FAIL [cache]: la segunda vez recompilo; el "
                            "artefacto comptime no se estaria reusando y el "
                            "ahorro seria cero\n");
            CK(a1.velb == a2.velb); // y es EL MISMO bytecode

            const vx::ComptimeArtifact a3 =
                vx::comptime_artifact_get(u2, cas, work);
            CK(a3.ok);
            CK(!a3.from_cache); // (c) cambiar la comptime NO reusa
            if (a3.ok && a3.from_cache)
                std::printf("FAIL [cache]: tras cambiar la funcion comptime se "
                            "reuso el artefacto: se ejecutaria codigo comptime "
                            "OBSOLETO\n");
        }
    }

    /* CASO 5 -- "no hay nada que hacer" NO es "no se pudo".  Un modulo sin
     * comptime devuelve ok=false pero SIN error: si se confundieran, un fallo
     * real de compilacion pasaria por "este modulo no tenia comptime". */
    {
        const std::string src = R"VX(
i64 suma(i64 a, i64 b) { return a + b; }
i32 main() { return 0; }
)VX";
        bool ok = false;
        const ComptimeUnit u = unidad_de(src, ok);
        CK(ok);
        if (ok) {
            const vx::CasStore cas((dir / "cas2").string());
            const vx::ComptimeArtifact a =
                vx::comptime_artifact_get(u, cas, (dir / "work2").string());
            CK(!a.ok);
            CK(a.error.empty());
            CK(a.velb.empty());
        }
    }

    /* CASO 6 -- CON `namespace`, que es donde se rompio de verdad.
     *
     * Al compilar un proyecto real, el conjunto extraido NO compilaba: daba
     * errores de SINTAXIS ("se esperaba ';' al final de la declaracion", "se
     * esperaba un tipo al inicio de la declaracion top-level").  Los casos de
     * arriba no lo cazaban porque ninguno usa `namespace`.
     *
     * La extraccion recorta cada decl por `[decl.offset, siguiente.offset)`.
     * Con un `namespace X { ... }` las decls de dentro llevan offsets dentro
     * del bloque, asi que un recorte puede arrastrar la llave de cierre -- o
     * perderla -- y lo extraido deja de ser codigo valido. */
    {
        const std::string src = R"VX(
namespace mimod;

i64 ayudante(i64 n) { return n * 3; }
comptime i64 usa_ayudante(i64 n) { return ayudante(n) + 1; }
i32 main() { return 0; }
)VX";
        bool ok = false;
        const ComptimeUnit u = unidad_de(src, ok);
        CK(ok);
        if (ok && !u.empty()) {
            vesta::tc::CompileResponse resp;
            const auto velb =
                compilar(u.unit_source, (dir / "ns").string(), resp);
            CK(resp.ok);
            if (!resp.ok) {
                std::printf(
                    "FAIL [namespace]: el conjunto extraido de un modulo"
                    " con `namespace` NO compila\n");
                std::printf("--- extraido ---\n%s\n---\n",
                            u.unit_source.c_str());
                volcar_diags(resp);
            }
            CK(!velb.empty());
        }
    }

    /* CASO 7 -- LA CONCATENACION DE VARIOS MODULOS, que es lo que hace el
     * compilador de proyecto y lo que fallaba de verdad.
     *
     * Cada modulo aporta su conjunto y todos se pegan en un solo fuente.  Al
     * hacerlo con un proyecto real salian errores de SINTAXIS, asi que lo que
     * hay que fijar no es que cada conjunto compile por separado -- eso ya lo
     * cubren los casos de arriba -- sino que la SUMA siga siendo codigo valido.
     *
     * Ojo al caso obvio que esto destapa: si dos modulos importan lo mismo, el
     * `import` sale DUPLICADO en la suma. */
    {
        const std::string mod_a = R"VX(
import std.io;
i64 ayudante_a(i64 n) { return n * 3; }
comptime i64 usa_a(i64 n) { return ayudante_a(n) + 1; }
i32 main() { return 0; }
)VX";
        const std::string mod_b = R"VX(
import std.io;
comptime i64 usa_b(i64 n) { return n * 7; }
)VX";
        bool oa = false, ob = false;
        const ComptimeUnit ua = unidad_de(mod_a, oa);
        const ComptimeUnit ub = unidad_de(mod_b, ob);
        CK(oa && ob);
        if (oa && ob) {
            const std::string suma = ua.unit_source + ub.unit_source;
            vesta::tc::CompileResponse resp;
            const auto velb = compilar(suma, (dir / "suma").string(), resp);
            CK(resp.ok);
            if (!resp.ok) {
                std::printf("FAIL [concatenacion]: la SUMA de los conjuntos de "
                            "dos modulos NO compila\n");
                std::printf("--- suma ---\n%s\n---\n", suma.c_str());
                volcar_diags(resp);
            }
            CK(!velb.empty());
        }
    }

    std::filesystem::remove_all(dir, ec);

    std::printf("\n=== test_comptime_artifact: %d OK, %d fallidos ===\n",
                g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
