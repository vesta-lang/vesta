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
 * @file test_threadpool.cpp
 * @brief Pruebas básicas para ThreadPool (sin framework externo).
 *
 * Ejecuta varias comprobaciones:
 *  - submit/return: tareas que devuelven valores
 *  - excepciones: tarea que lanza y future.get() propaga la excepción
 *  - concurrencia: N tareas incrementando un contador atómico
 *  - shutdown: no se permiten nuevas tareas tras shutdown
 *
 * Imprime mensajes informativos y al final un resumen con PASS/FAIL.
 */

#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <cassert>

#include "util/ThreadPool.h"

static void print_header(const char *title) {
    std::cout << "==================== " << title << " ====================\n";
}

int main() {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
    system("chcp 65001");
#endif

    bool all_ok = true;

    // 1) Test básico: submit y obtener resultados
    print_header("TEST 1: submit y obtener resultados");
    try {
        ThreadPool pool(4);
        std::vector<std::future<int> > futs;
        for (int i = 0; i < 8; ++i) {
            futs.push_back(pool.submit([i]() {
                // simular trabajo
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return i * 2;
            }));
        }

        bool ok = true;
        for (int i = 0; i < (int) futs.size(); ++i) {
            int r = futs[i].get();
            std::cout << "task " << i << " -> " << r << "\n";
            if (r != i * 2) ok = false;
        }
        std::cout << (ok ? "TEST 1 PASS\n" : "TEST 1 FAIL\n");
        all_ok &= ok;
        pool.shutdown();
    } catch (const std::exception &e) {
        std::cout << "EXCEPCION en TEST 1: " << e.what() << "\n";
        all_ok = false;
    }

    // 2) Test de propagación de excepciones
    print_header("TEST 2: excepcion en tarea y propagacion por future");
    try {
        ThreadPool pool(2);
        auto fut_ok = pool.submit([]() { return std::string("ok"); });
        auto fut_err = pool.submit([]() -> std::string {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            throw std::runtime_error("error intencional");
            return "never";
        });

        bool ok = true;
        try {
            std::string s = fut_ok.get();
            std::cout << "fut_ok -> " << s << "\n";
        } catch (...) {
            std::cout << "fut_ok lanzó excepción inesperada\n";
            ok = false;
        }

        try {
            std::string s2 = fut_err.get();
            (void) s2;
            std::cout << "ERROR: fut_err no lanzó excepción\n";
            ok = false;
        } catch (const std::exception &e) {
            std::cout << "fut_err propagó excepción: " << e.what() << "\n";
        } catch (...) {
            std::cout << "fut_err propagó excepción no std::exception\n";
        }

        std::cout << (ok ? "TEST 2 PASS\n" : "TEST 2 FAIL\n");
        all_ok &= ok;
        pool.shutdown();
    } catch (const std::exception &e) {
        std::cout << "EXCEPCION en TEST 2: " << e.what() << "\n";
        all_ok = false;
    }

    // 3) Test de concurrencia: N tareas incrementando contador atómico
    print_header("TEST 3: concurrencia y conteo atómico");
    try {
        ThreadPool pool(4);
        std::atomic<int> counter{0};
        const int N = 200;
        std::vector<std::future<void> > futs;
        for (int i = 0; i < N; ++i) {
            futs.push_back(pool.submit([&counter]() {
                // trabajo variable
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                counter.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        for (auto &f: futs) f.get();
        bool ok = (counter.load() == N);
        std::cout << "counter = " << counter.load() << " (expected " << N << ")\n";
        std::cout << (ok ? "TEST 3 PASS\n" : "TEST 3 FAIL\n");
        all_ok &= ok;
        pool.shutdown();
    } catch (const std::exception &e) {
        std::cout << "EXCEPCION en TEST 3: " << e.what() << "\n";
        all_ok = false;
    }

    // 4) Test shutdown: no se aceptan nuevas tareas tras shutdown
    print_header("TEST 4: shutdown y rechazo de nuevas tareas");
    try {
        ThreadPool pool(2);
        // Encolar una tarea larga
        auto fut = pool.submit([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return 42;
        });

        // Llamar shutdown mientras la tarea está en curso
        pool.shutdown();

        bool ok = true;
        // La tarea en curso debe completarse (fut.get() ok)
        try {
            int r = fut.get();
            std::cout << "tarea en curso completada -> " << r << "\n";
        } catch (const std::exception &e) {
            std::cout << "ERROR: tarea en curso lanzó excepción: " << e.what() << "\n";
            ok = false;
        }

        // Intentar enviar nueva tarea debe lanzar runtime_error
        bool threw = false;
        try {
            auto fut2 = pool.submit([]() { return 1; });
            (void) fut2;
            std::cout << "ERROR: submit después de shutdown no lanzó\n";
            ok = false;
        } catch (const std::exception &e) {
            std::cout << "submit tras shutdown lanzó: " << e.what() << "\n";
            threw = true;
        }
        if (!threw) ok = false;

        std::cout << (ok ? "TEST 4 PASS\n" : "TEST 4 FAIL\n");
        all_ok &= ok;
    } catch (const std::exception &e) {
        std::cout << "EXCEPCION en TEST 4: " << e.what() << "\n";
        all_ok = false;
    }

    // Resumen final
    print_header("RESUMEN");
    if (all_ok) {
        std::cout << "TODAS LAS PRUEBAS PASARON (OK)\n";
        return 0;
    } else {
        std::cout << "ALGUNAS PRUEBAS FALLARON\n";
        return 2;
    }
}
