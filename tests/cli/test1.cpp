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

#include "cli/runtime_api_commands.h"

#include <iostream>

#include "cli/cli.h"
#include "cli/sync_io.h"



int main() {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
    system("chcp 65001");
#endif
    // Inicializar runtime con 4 hilos
    runtime::runtime_init(4);

    cli::Config cfg;
    cfg.prompt = "vesta> ";
    cli::VestaViewManager vm(cfg);

    // Callback que usa run_command_async y muestra resultado cuando termine
    vm.set_execute_callback([](const std::string &cmd){
        auto token = runtime::make_cancel_token();
        auto fut = runtime::run_command_async(cmd, token);
        std::thread([f = std::move(fut)]() mutable {
            try {
                std::string out = f.get();
                if (!out.empty()) {
                    // Usar el mutex global para sincronizar la salida
                    std::lock_guard lk(vesta::cout_mutex);
                    std::cout << out << std::endl;
                }
            } catch (const std::exception &e) {
                std::lock_guard lk(vesta::cout_mutex);
                std::cerr << "Error en comando: " << e.what() << std::endl;
            }
        }).detach();
    });

    vm.run();

    // Shutdown ordenado
    runtime::runtime_shutdown();
    return 0;
}