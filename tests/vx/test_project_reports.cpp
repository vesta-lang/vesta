/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/vx/test_project_reports.cpp
 * @brief Comprueba que el camino de PROYECTO devuelve lo mismo que el de
 *        fichero suelto en lo que el servidor de lenguaje ensena: el
 *        diagrama de tipos y lo que se sabe de cada `@Macro`.
 *
 * Un fichero que declara `namespace` se compila como proyecto.  Ese camino
 * no sacaba el diagrama de tipos ni recogia las llamadas a `@Macro`, asi que
 * el editor se quedaba en blanco en cuanto el documento tenia un `namespace`
 * o un `import`.  Se compila el MISMO programa por los dos caminos y se
 * comparan.
 */
#include "vx/compiler.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int g_failures = 0; ///< Cuantas comprobaciones han fallado.

/// El programa: un struct para el diagrama de tipos y un `@Macro` llamado con
/// un argumento entero, que es lo que el comprobador registra.
const char *const kProgram = "i32 doblar(i32 x) => x * 2;\n"
                             "struct Point {\n"
                             "\ti64 x;\n"
                             "\ti64 y;\n"
                             "}\n"
                             "@Macro\n"
                             "comptime string macro_double(i64 n) => "
                             "\"doblar(\" + to_str(n) + \")\";\n"
                             "i32 main() {\n"
                             "\ti32 r = macro_double(21);\n"
                             "\treturn r;\n"
                             "}\n";

/**
 * @brief Deja constancia de un fallo si @p ok es falso.
 * @param ok   La condicion.
 * @param what Que se estaba comprobando.
 */
void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FALLO: %s\n", what);
        ++g_failures;
    }
}

/**
 * @brief Busca, entre las llamadas a `@Macro` resueltas, la que genero
 *        @p code.
 * @param res  El resultado de compilar.
 * @param code El codigo generado que se espera.
 * @return Si esta.
 */
bool has_expansion(const vx::CompileResult &res, const std::string &code) {
    for (const auto &e : res.macro_expectations)
        if (e.expected_str.find(code) != std::string::npos) return true;
    return false;
}

/**
 * @brief Las opciones con que se compila: el diagrama de tipos pedido.
 * @return Las opciones.
 */
vx::CompileOptions report_options() {
    vx::CompileOptions opts;
    opts.module_name = "main";
    opts.dump_mermaid_types = true;
    return opts;
}

} // namespace

/**
 * @brief Punto de entrada del test.
 * @return 0 si todo paso, 1 si hubo algun fallo.
 */
int main() {
    namespace fs = std::filesystem;
    const vx::CompileOptions opts = report_options();

    // Fichero suelto: la referencia.
    const vx::CompileResult single =
        vx::compile_vx_source(kProgram, "reports_single.vx", opts);
    check(single.ok, "el fichero suelto compila");
    check(single.mermaid_types.find("Point") != std::string::npos,
          "el fichero suelto dibuja el struct");
    check(has_expansion(single, "doblar(21)"),
          "el fichero suelto registra la llamada al @Macro");

    // El mismo programa con `namespace`: va por el camino de proyecto.
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "vesta_test_reports";
    fs::create_directories(dir, ec);
    const fs::path root = dir / "reports_project.vx";
    {
        std::ofstream f(root, std::ios::binary);
        f << "namespace reports.project;\n" << kProgram;
    }
    const vx::CompileResult project =
        vx::compile_vx_project(root.string(), opts);
    check(project.ok, "el proyecto compila");
    check(project.mermaid_types.find("Point") != std::string::npos,
          "el proyecto dibuja el struct");
    check(has_expansion(project, "doblar(21)"),
          "el proyecto registra la llamada al @Macro");
    fs::remove_all(dir, ec);

    if (g_failures == 0) std::printf("test_project_reports: OK\n");
    return g_failures == 0 ? 0 : 1;
}
