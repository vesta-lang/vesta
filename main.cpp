/*
* VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

#include "assembly/assembly.h"

#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <openssl/sha.h>

#include "json.hpp"
#include "cxxopts.hpp"
#include "cli/cli.h"
#include "cli/runtime_api_commands.h"
#include "util/sqlite_singleton.h"

using json = nlohmann::json;

int main(int argc, char *argv[]) {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
    system("chcp 65001");
#endif

    cxxopts::Options options("VMProject", "Virtual Machine Example");

    options.add_options()
            ("h,help", "Mostrar ayuda")
            ("v,version", "Mostrar versión")
            ("m,mode", "Modo de ejecución (vm/jit)", cxxopts::value<std::string>()->default_value("vm"))
            ("list-arch", "Imprimir arquitecturas soportadas")
            ("asm-file", "Archivo ASM a ensamblar", cxxopts::value<std::string>())
            ("disasm-file", "Archivo binario a desensamblar", cxxopts::value<std::string>())
            ("arch", "Arquitectura para ensamblar/desensamblar", cxxopts::value<std::string>())
            ("save-output", "Guardar código ensamblado/desensamblado en archivo")
            ("output-prefix", "Prefijo/nombre base para archivos de salida",
             cxxopts::value<std::string>()->default_value("out"));

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    if (result.count("version")) {
        std::cout << "VMProject v0.1.0" << std::endl;
        return 0;
    }

    if (result.count("list-arch")) {
        const ArchSupport &archs = get_available_architectures();
        std::cout << "Capstone supported architectures:\n";
        for (auto &a: archs.capstone) std::cout << "  " << a << "\n";
        std::cout << "Keystone supported architectures:\n";
        for (auto &a: archs.keystone) std::cout << "  " << a << "\n";
        return 0;
    }

    bool save_output = result.count("save-output") > 0;
    std::string prefix = result["output-prefix"].as<std::string>();

    if (result.count("asm-file")) {
        if (!result.count("arch")) {
            std::cerr << "--arch es requerido para ensamblar\n";
            return EXIT_FAILURE;
        }
        return assemble_file(
                   result["asm-file"].as<std::string>(),
                   result["arch"].as<std::string>(),
                   save_output,
                   prefix
               )
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }

    if (result.count("disasm-file")) {
        if (!result.count("arch")) {
            std::cerr << "--arch es requerido para desensamblar\n";
            return EXIT_FAILURE;
        }
        return disassemble_file(
                   result["disasm-file"].as<std::string>(),
                   result["arch"].as<std::string>(),
                   save_output,
                   prefix
               )
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }


    cli::Config cfg;
    cfg.history_file = "my_vm_history.txt";
    cfg.history_max = 1000;
    cfg.prompt = "vesta> ";
    cfg.multiline_end = ";;";

    cli::VestaViewManager vm(cfg);

    vm.set_execute_callback([](const std::string &cmd) {
        auto fut = runtime::run_command_async(cmd);
        std::thread([f = std::move(fut)]() mutable {
            try {
                auto out = f.get();
                if (!out.empty()) std::cout << out << std::endl;
            } catch (const std::exception &e) {
                std::cerr << "Runtime error: " << e.what() << std::endl;
            }
        }).detach();
    });

    vm.run();


    return EXIT_SUCCESS;
}
