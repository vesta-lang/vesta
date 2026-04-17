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

#include "util/ThreadPool.h"

#include <iostream>
#include <utility>

ThreadPool::ThreadPool(size_t n) {
    if (n == 0) {
        n = std::max<size_t>(1, std::thread::hardware_concurrency());
    }
    workers_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        workers_.emplace_back([this] {
            this->worker_loop();
        });
    }
}

ThreadPool::~ThreadPool() {
    //std::cout << "Destruyendo ThreadPool\n";
    shutdown();
}

void ThreadPool::shutdown() {
    wake_flag_.store(1, std::memory_order_release);

#ifdef WIN32
    WakeByAddressAll(&wake_flag_);
#else
    futex_wake(&wake_flag_, workers_.size());
#endif

}

bool ThreadPool::idle() {
    // No tareas pendientes y ningún worker ejecutando nada
    std::lock_guard lk(tasks_m_);
    return tasks_.empty();
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;

        while (true) {
            int expected = 0;

            // Comprobamos tareas SIN dormir con el mutex
            {
                std::lock_guard lk(tasks_m_);
                if (!tasks_.empty()) break;
                if (stopping_.load()) return;
            }

            // Resetear flag antes de dormir
            wake_flag_.store(0, std::memory_order_relaxed);

#ifdef WIN32
            WaitOnAddress(&wake_flag_, &expected, sizeof(int), INFINITE);
#else
            futex_wait(&wake_flag_, expected);
#endif
        }

        // Extraer tarea
        {
            std::lock_guard lk(tasks_m_);
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
            }
        }

        if (task) task();
    }
}
