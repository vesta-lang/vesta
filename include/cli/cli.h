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
#ifndef CLI_H
#define CLI_H

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <atomic>
#include <algorithm>
#include <csignal>
#include <functional>
#include <sstream>
#include <filesystem>
#include <cstdlib>   // getenv

#include "runtime/manager_runtime.h"
#include "runtime/runtime.h"


namespace cli {
    struct Impl;

    struct Config {
        std::string history_file = "vm_history.txt";
        size_t history_max = 2000;
        std::string prompt = "vm> ";
        std::string multiline_end = ";;";
    };

    class VestaViewManager {
    public:

        explicit VestaViewManager(Config cfg = Config());

        ~VestaViewManager();

        // Inyecta la función que ejecuta comandos en la VM
        void set_execute_callback(std::function<void(const std::string &)> cb);

        // Inicia el REPL (bloqueante). Devuelve al salir.
        void run();

        // Detiene el manager y espera al worker
        void stop();


    private:
        // No copiar ni mover
        VestaViewManager(const VestaViewManager &) = delete;

        VestaViewManager &operator=(const VestaViewManager &) = delete;

        // Implementación interna
        Impl *impl_;
    };
}

#endif
