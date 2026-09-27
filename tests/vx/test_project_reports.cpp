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
                             "comptime auto REPORT_SIZE = 6 * 7;\n"
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
 * @brief Busca, entre los valores comptime publicados, uno cuyo nombre
 *        contenga @p name y cuyo valor sea @p value.
 * @param res   El resultado de compilar.
 * @param name  Parte del nombre (con `namespace` va aplanado).
 * @param value El valor escrito.
 * @return Si esta.
 */
bool has_comptime_value(const vx::CompileResult &res, const std::string &name,
                        const std::string &value) {
    for (const auto &v : res.comptime_values)
        if (v.name.find(name) != std::string::npos && v.value_str == value)
            return true;
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
    opts.dump_comptime_values = true;
    return opts;
}

/// Un contrato de tipo INCUMPLIDO: el struct mide 16 bytes y declara 8.
const char *const kBrokenTypeContract = "@size(8)\n"
                                        "struct Wide {\n"
                                        "\tf64 x;\n"
                                        "\tf64 y;\n"
                                        "}\n"
                                        "i32 main() { return 0; }\n";

/**
 * @brief Si @p res tiene un error con el codigo @p code.
 * @param res  El resultado de compilar.
 * @param code El codigo del catalogo.
 * @return Si esta.
 */
bool has_error(const vx::CompileResult &res, const char *code) {
    for (const auto &d : res.diagnostics.all())
        if (d.level == vx::DiagLevel::ERR && d.code == code) return true;
    return false;
}

/**
 * @brief Si la huella de @p type_name esta en el resultado, con @p size bytes.
 * @param res       El resultado de compilar.
 * @param type_name Parte del nombre del tipo (con `namespace` va aplanado).
 * @param size      Su tamano esperado.
 * @return Si esta.
 */
bool has_fingerprint(const vx::CompileResult &res, const std::string &type_name,
                     uint64_t size) {
    for (const auto &tf : res.type_fingerprints)
        if (tf.type_name.str().find(type_name) != std::string::npos &&
            tf.size_bytes == size)
            return true;
    return false;
}

/**
 * @brief Los contratos de TIPO se comprueban por los DOS caminos.  El de
 *        proyecto no los comprobaba: un `@size` incumplido compilaba.
 * @param dir Directorio temporal para el fichero del proyecto.
 */
void check_type_contracts(const std::filesystem::path &dir) {
    const vx::CompileResult single = vx::compile_vx_source(
        kBrokenTypeContract, "broken_single.vx", report_options());
    check(!single.ok, "el fichero suelto rechaza el @size incumplido");
    check(has_error(single, "VXT004"), "el fichero suelto lo dice (VXT004)");
    check(has_fingerprint(single, "Wide", 16),
          "el fichero suelto publica la huella del tipo");

    const std::filesystem::path root = dir / "broken_project.vx";
    {
        std::ofstream f(root, std::ios::binary);
        f << "namespace reports.broken;\n" << kBrokenTypeContract;
    }
    const vx::CompileResult project =
        vx::compile_vx_project(root.string(), report_options());
    check(!project.ok, "el proyecto rechaza el @size incumplido");
    check(has_error(project, "VXT004"), "el proyecto lo dice (VXT004)");
    check(has_fingerprint(project, "Wide", 16),
          "el proyecto publica la huella del tipo");
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
    check(has_comptime_value(single, "REPORT_SIZE", "42"),
          "el fichero suelto publica el valor comptime");

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
    check(has_comptime_value(project, "REPORT_SIZE", "42"),
          "el proyecto publica el valor comptime");
    check_type_contracts(dir);
    fs::remove_all(dir, ec);

    if (g_failures == 0) std::printf("test_project_reports: OK\n");
    return g_failures == 0 ? 0 : 1;
}
