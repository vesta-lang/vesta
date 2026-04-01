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

#include <utility>

ThreadPool::ThreadPool(size_t n) {
    if (n == 0) {
        n = std::max<size_t>(1, std::thread::hardware_concurrency());
    }
    workers_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        workers_.emplace_back([this]{ this->worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) return; // ya cerrando
    {
        std::lock_guard lk(tasks_m_);
        // marcar cerrado; no se añaden más tareas
    }
    tasks_cv_.notify_all();
    for (auto &t : workers_) if (t.joinable()) t.join();
    workers_.clear();
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lk(tasks_m_);
            tasks_cv_.wait(lk, [this]{ return stopping_.load() || !tasks_.empty(); });
            if (stopping_.load() && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            task();
        } catch (...) {
            // No propagar excepción fuera del hilo worker.
            // Opcional: loggear?
        }
    }
}

