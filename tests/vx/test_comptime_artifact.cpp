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
            std::printf("FAIL linea %d: %s\n", __LINE__, #cond);                \
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
     * y que, ademas, llama a algo inexistente: si el extractor se llevara de mas,
     * la compilacion de la unidad fallaria y el test lo diria. */
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
                std::printf("FAIL [unidad]: el conjunto comptime NO compila por "
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
            std::printf("FAIL [contraste]: el fuente completo compilo, asi que "
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

    std::filesystem::remove_all(dir, ec);

    std::printf("\n=== test_comptime_artifact: %d OK, %d fallidos ===\n", g_pass,
                g_fail);
    return g_fail == 0 ? 0 : 1;
}
