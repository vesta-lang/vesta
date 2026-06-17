/**
 * @file interprete.cpp
 * @brief Implementacion del interprete interactivo @c VestaInterprete del CLI.
 *
 * Implementa @c cli::VestaInterprete::run_interprete() que lexifica, parsea
 * y ensambla codigo fuente introducido desde el REPL, ejecutandolo de forma
 * incremental en una instancia VM interna.
 *
 * Ciclo de ejecucion incremental (por fragmento):
 *   1. Leer linea de codigo Vesta.
 *   2. Ensamblar a bytecode con HLT al final.
 *   3. Esperar a que el scheduler anterior haya terminado (f.wait()).
 *   4. Limpiar el icache del proceso (reset_cache).
 *   5. Cargar bytecode en la VM a partir del RIP actual (load_raw_code).
 *   6. Poner el proceso listo (make_ready): incrementa alive_count, anota
 *      should_kill=false y libera el semaforo del scheduler.
 *   7. Lanzar el scheduler (start): AHORA que el proceso ya esta en la cola.
 *   8. Esperar a que el proceso llegue a HALT o DEAD.
 *   9. Leer el nuevo RIP para la siguiente iteracion.
 *
 * NOTA: vm->start() se llama DESPUES de make_ready() en cada iteracion.
 * Si se llamase antes, el scheduler podria detectar alive_count==0 y terminar
 * antes de que el proceso llegue a la cola, causando que no se ejecute nada.
 */
#include "cli/cli.h"

#include <cstdio>
#include <thread>
#include <chrono>

#include "cli/cli_init_manager_and_server.h"
#include "cli/sync_io.h"
#include "emmit/parser_to_bytecode.h"
#include "util/ansi.h"

namespace cli {
using namespace Assembly::Bytecode;

VestaInterprete::VestaInterprete() {}

std::vector<uint8_t> VestaInterprete::run_interprete_execute_code() {
    vm::Lexer lexer(code);
    vm::Parser parser(lexer);
    Assembler asmblr;
    return asmblr.assemble(parser.parse());
}

std::vector<std::string> VestaInterprete::tokenize(const std::string &input) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream iss(input);
    while (iss >> token) {
        std::transform(token.begin(), token.end(), token.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        tokens.push_back(token);
    }
    return tokens;
}

/**
 * @brief Bucle principal del interprete interactivo de Vesta.
 *
 * Crea una instancia VM con un scheduler y entra en bucle de
 * lectura/ensamblado/ejecucion.  Cada fragmento ensamblado termina con HLT;
 * al llegar a HALT el scheduler se relanza para el siguiente fragmento.
 * El estado de los registros se preserva entre iteraciones.
 *
 * Comandos internos:
 *   - quit        : salir del interprete
 *   - bytecode on : mostrar bytecode ensamblado
 *   - bytecode off: ocultar bytecode ensamblado
 *   - status      : mostrar estado de la VM
 */
void VestaInterprete::run_interprete() {
    ansi::init(); // habilitar colores si el terminal lo soporta

    uint8_t id = manager.create_vm(1);
    runtime::VM *vm = manager.get_vm(id);
    // NO llamar vm->start() aqui: se llama dentro del bucle, tras make_ready,
    // para garantizar que el proceso ya esta en la cola cuando el scheduler
    // arranca.

    GlobalPID pid = vm->spawn_process();
    runtime::ProcessVM *process = vm->get_process(pid);
    uint64_t last_ip = 0; // direccion donde se cargara el siguiente fragmento

    vesta::scout() << ansi::c(ansi::BR_CYAN) << "[interprete]"
                   << ansi::c(ansi::RESET)
                   << " Modo interactivo. Escribe 'quit' para salir, 'status' "
                      "para ver la VM.\n";

    while (running) {
        try {
            // prompt del interprete con color
            vesta::scout() << ansi::c(ansi::BOLD) << ansi::c(ansi::CYAN)
                           << "vesta[interprete]{" << (int)id << "}> "
                           << ansi::c(ansi::RESET);

            if (!std::getline(std::cin, code)) break; // EOF

            // trim leading/trailing whitespace
            auto &s = code;
            s.erase(0, s.find_first_not_of(" \t"));
            if (!s.empty()) s.erase(s.find_last_not_of(" \t") + 1);

            auto tokens = tokenize(code);
            if (tokens.empty()) continue;

            // --- comandos internos ---
            if (tokens[0] == "quit") break;

            if (tokens[0] == "bytecode") {
                if (tokens.size() == 2 && tokens[1] == "on") {
                    show_bytecode = true;
                    std::cout << ansi::c(ansi::GREEN) << "[info] bytecode ON\n"
                              << ansi::c(ansi::RESET);
                } else if (tokens.size() == 2 && tokens[1] == "off") {
                    show_bytecode = false;
                    std::cout << ansi::c(ansi::YELLOW)
                              << "[info] bytecode OFF\n"
                              << ansi::c(ansi::RESET);
                } else {
                    std::cout << ansi::c(ansi::RED)
                              << "[error] uso: bytecode on | bytecode off\n"
                              << ansi::c(ansi::RESET);
                }
                continue;
            }

            if (tokens[0] == "status") {
                std::cout << vm->to_string() << "\n";
                continue;
            }

            // --- ensamblar ---
            std::vector<uint8_t> bytecode = run_interprete_execute_code();

            if (show_bytecode)
                vesta::scout()
                    << vesta::dump(bytecode.data(), bytecode.size()) << "\n";

            // agregar HLT al final para que el scheduler detenga el fragmento
            bytecode.push_back(0x00);
            bytecode.push_back(0x03);

            // esperar a que el scheduler anterior haya terminado completamente
            // (puede estar todavia en run_loop despues de que el proceso llegue
            // a HALT)
            for (auto &f : vm->scheduler_futures) {
                if (f.valid()) f.wait();
            }
            vm->scheduler_futures.clear(); // limpiar futures consumidos

            // invalidar el icache para que el decodificador no reutilice
            // instrucciones del fragmento anterior que ocupara la misma
            // direccion de memoria
            process->reset_cache();

            // cargar bytecode en memoria del proceso a partir del IP actual
            process->load_raw_code(last_ip, bytecode);

            // make_ready: incrementa alive_count, pone state=READY,
            // should_kill=false y libera el semaforo del scheduler
            vm->make_ready(pid);

            // lanzar el scheduler AHORA que el proceso ya esta en la cola FIFO.
            // si se llamase antes, el scheduler podria ver alive_count==0 y
            // terminar.
            vm->start();

            // esperar a que el proceso complete el fragmento (HALT) o falle
            // (DEAD)
            while (process->state != runtime::HALT &&
                   process->state != runtime::DEAD) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            // leer RIP tras HLT: apunta al byte siguiente al HLT,
            // que es donde el proximo fragmento debe cargarse para preservar
            // estado
            last_ip = process->registers.rip.raw();

        } catch (const std::exception &e) {
            std::cerr << ansi::c(ansi::BR_RED) << "[error] " << e.what()
                      << ansi::c(ansi::RESET) << "\n";
        }
    }

    // detener la VM limpiamente (espera a que el scheduler actual termine)
    vm->stop();
    manager.destroy_all_vms();
}
} // namespace cli
