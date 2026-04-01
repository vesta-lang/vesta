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
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <thread>
#include <functional>
#include <future>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

/**
 * @brief Simple thread pool para ejecutar tareas y devolver futures.
 *
 * Implementación mínima, segura y suficiente para encolar ejecuciones de comandos.
 * No depende de librerías externas.
 */
class ThreadPool {
public:
    /**
     * @brief Construye el pool con n hilos. Si n == 0 usa hardware_concurrency() o 1.
     * @param n número de hilos worker
     */
    explicit ThreadPool(size_t n = 0);

    /**
     * @brief Destructor: espera a que terminen las tareas y detiene los hilos.
     */
    ~ThreadPool();

    // No copiar ni mover
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief Encola una tarea y devuelve un std::future con el resultado.
     * @tparam F callable
     * @tparam Args argumentos
     * @param f callable
     * @param args argumentos
     * @return std::future<R> donde R es el tipo devuelto por f(args...)
     */
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<typename std::invoke_result_t<F, Args...>>;

    /**
     * @brief Solicita el cierre del pool: no acepta nuevas tareas y espera a las pendientes.
     */
    void shutdown();

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex tasks_m_;
    std::condition_variable tasks_cv_;
    std::atomic<bool> stopping_{false};
};

#endif //THREADPOOL_H
