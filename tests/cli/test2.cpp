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

/**
 * @file test2.cpp
 * @brief Ejemplo de integración síncrona entre VestaViewManager y runtime.
 *
 * Este ejemplo usa run_command_sync para ejecutar comandos de la VM de forma
 * síncrona desde el hilo worker del VestaViewManager. La salida se imprime
 * protegida por un mutex global (vesta::cout_mutex) para evitar mezclas de texto
 * cuando varios hilos escriben a la consola.
 *
 * Comportamiento:
 * - El REPL encola comandos.
 * - El hilo worker consume comandos y llama al callback inyectado.
 * - El callback llama a runtime::run_command_sync(cmd) y bloquea hasta que
 *   la ejecución termine (o lance excepción).
 *
 * Nota: bloquear el worker no bloquea la UI principal (prompt), pero impide
 * procesar otros comandos en paralelo en el mismo worker. Si necesitas
 * paralelismo, usa la versión asíncrona con thread pool.
 */

#include "cli/runtime_api_commands.h"
#include "cli/cli.h"
#include "cli/sync_io.h"

#include <iostream>
#include <thread>
#include <exception>

int main() {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
    system("chcp 65001");
#endif
    // Inicializar runtime con 4 hilos (pool interno del runtime)
    runtime::runtime_init(4);

    // Configuración del REPL
    cli::Config cfg;
    cfg.prompt = "vesta> ";
    cli::VestaViewManager vm(cfg);

    /**
     * @brief Callback síncrono para ejecutar comandos en la VM.
     *
     * Este callback será invocado por el hilo worker de VestaViewManager.
     * Llama a runtime::run_command_sync, captura excepciones y sincroniza
     * la impresión con vesta::cout_mutex.
     *
     * @param cmd Comando textual a ejecutar.
     */
    vm.set_execute_callback([](const std::string &cmd) {
        try {
            // Ejecutar de forma síncrona en el runtime.
            // El worker que llamó a este callback quedará bloqueado hasta que
            // la ejecución termine o lance una excepción.
            std::string out = runtime::run_command_sync(cmd);

            // Imprimir resultado protegido por mutex global para evitar
            // mezclas con otras salidas concurrentes.
            if (!out.empty()) {
                std::lock_guard lk(vesta::cout_mutex);
                std::cout << out << std::endl;
            }
        } catch (const std::exception &e) {
            // Imprimir error de forma segura
            std::lock_guard lk(vesta::cout_mutex);
            std::cerr << "Error en ejecucion sincrona: " << e.what() << std::endl;
        } catch (...) {
            std::lock_guard lk(vesta::cout_mutex);
            std::cerr << "Error desconocido en ejecucion sincrona" << std::endl;
        }
    });

    // Ejecutar el REPL (bloqueante hasta que el usuario salga con "exit")
    vm.run();

    // Shutdown ordenado del runtime y su thread pool
    runtime::runtime_shutdown();
    return 0;
}
