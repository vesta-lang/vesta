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
#include <thread>
#include <chrono>
#include <stdexcept>
#include <iostream>
#include <future>
#include <memory>

namespace runtime {
    // Pool global del runtime (archivo-scope)
    static std::unique_ptr<ThreadPool> g_pool;
    static std::mutex g_pool_m;

    CancelToken make_cancel_token() {
        return std::make_shared<std::atomic<bool> >(false);
    }

    void runtime_init(size_t threads) {
        std::lock_guard lk(g_pool_m);
        if (!g_pool) g_pool = std::make_unique<ThreadPool>(threads);
    }

    void runtime_shutdown() {
        std::lock_guard lk(g_pool_m);
        if (g_pool) {
            g_pool->shutdown();
            g_pool.reset();
        }
    }

    /* Stub de ejecución real; reemplazar por llamada a ManagerRuntime u otro motor. */
    static std::string execute_on_engine(const std::string &cmd, CancelToken cancel_token) {
        const int steps = 5;
        for (int i = 0; i < steps; ++i) {
            if (cancel_token && cancel_token->load()) {
                throw std::runtime_error("execution cancelled");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return std::string("[runtime stub] salida de: ") + cmd;
    }

    std::string run_command_sync(const std::string &cmd,
                                 std::chrono::milliseconds timeout,
                                 CancelToken cancel_token) {
        if (timeout == std::chrono::milliseconds::zero()) {
            return execute_on_engine(cmd, cancel_token);
        }
        auto fut = std::async(std::launch::async, [&cmd, cancel_token]() {
            return execute_on_engine(cmd, cancel_token);
        });
        if (fut.wait_for(timeout) == std::future_status::ready) {
            return fut.get();
        } else {
            if (cancel_token) cancel_token->store(true);
            throw std::runtime_error("timeout waiting for command execution");
        }
    }

    std::future<std::string> run_command_async(const std::string &cmd,
                                               CancelToken cancel_token) {
        // Asegurar pool inicializado
        {
            std::lock_guard lk(g_pool_m);
            if (!g_pool) g_pool = std::make_unique<ThreadPool>(0);
        }
        // Encolar la tarea en el pool y devolver future
        return g_pool->submit([cmd, cancel_token]() {
            return execute_on_engine(cmd, cancel_token);
        });
    }
} // namespace runtime
