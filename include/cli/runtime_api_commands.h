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

#ifndef RUNTIME_API_COMMANDS_H
#define RUNTIME_API_COMMANDS_H

#include "util/ThreadPool.h"

#include <string>
#include <future>
#include <chrono>
#include <memory>
#include <atomic>

namespace runtime {
    using CancelToken = std::shared_ptr<std::atomic<bool>>;

    CancelToken make_cancel_token();

    /**
     * Ejecuta un comando de forma sincrona.
     */
    std::string run_command_sync(const std::string &cmd,
                                 std::chrono::milliseconds timeout = std::chrono::milliseconds::zero(),
                                 CancelToken cancel_token = nullptr);

    /**
     * Ejecuta un comando de forma asincrona usando el thread pool interno.
     * Devuelve un future que contendrá la salida o excepción.
     */
    std::future<std::string> run_command_async(const std::string &cmd,
                                               CancelToken cancel_token = nullptr);

    /**
     * Inicializa el thread pool del runtime (opcional).
     * Si no se llama, se crea uno interno con hardware_concurrency() hilos.
     */
    void runtime_init(size_t threads = 0);

    /**
     * Cierra el runtime y su thread pool (espera tareas pendientes).
     */
    void runtime_shutdown();
}
#endif //RUNTIME_API_COMMANDS_H
