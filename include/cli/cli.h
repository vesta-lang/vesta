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
#include <optional>
#include <iostream>

#include "runtime/manager_runtime.h"
#include "runtime/runtime.h"
#include "util/fs_utils.h"

namespace cli {
    struct Impl;

    struct CmdParams {
        std::string name; ///< primer token (nombre)
        std::string path; ///< segundo token (ruta al ejecutable)
        std::string rest; ///< resto de la cadena (opcional)
        bool valid = false; ///< true si se extrajeron al menos name y path
    };

    /**
     * @brief Extrae un token de la cadena empezando en pos, respetando comillas y escapes.
     *
     * @param s Cadena de entrada.
     * @param pos Posición inicial (se actualiza al final del token).
     * @return token extraído.
     */
    static std::string extract_token(const std::string &s, size_t &pos) {
        // saltar espacios iniciales
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos >= s.size()) return {};

        std::string out;
        char quote = 0;
        if (s[pos] == '"' || s[pos] == '\'') {
            quote = s[pos++];
        }

        while (pos < s.size()) {
            char c = s[pos++];
            if (quote) {
                if (c == '\\' && pos < s.size()) {
                    // escape dentro de comillas
                    char next = s[pos++];
                    out.push_back(next);
                } else if (c == quote) {
                    // fin de comillas
                    break;
                } else {
                    out.push_back(c);
                }
            } else {
                if (std::isspace(static_cast<unsigned char>(c))) {
                    break;
                } else if (c == '\\' && pos < s.size()) {
                    // escape fuera de comillas (permite espacios escapados)
                    char next = s[pos++];
                    out.push_back(next);
                } else {
                    out.push_back(c);
                }
            }
        }

        return out;
    }

    /**
     * @brief Parsea la cadena cmd extrayendo name (primer token) y path (segundo token).
     *
     * @param cmd Cadena completa del comando.
     * @return CmdParams con name, path, rest y valid=true si name+path fueron extraídos.
     *
     * @code{.cpp}
     * // Ejemplos de uso:
     * auto p1 = parse_cmd_params(R"(myname /home/user/vm.bin)");
     * // p1.name == "myname", p1.path == "/home/user/vm.bin"
     *
     * auto p2 = parse_cmd_params(R"("My Name" "C:\Program Files\VM\vm.exe" --flag)");
     * // p2.name == "My Name", p2.path == "C:\Program Files\VM\vm.exe", p2.rest == "--flag"
     * @endcode
     */
    static CmdParams parse_cmd_params(const std::string &cmd) {
        CmdParams out;
        size_t pos = 0;
        out.name = extract_token(cmd, pos);
        if (out.name.empty()) return out; // invalid

        out.path = extract_token(cmd, pos);
        if (out.path.empty()) return out; // invalid

        // resto de la cadena: saltar espacios y tomar el resto tal cual
        while (pos < cmd.size() && std::isspace(static_cast<unsigned char>(cmd[pos]))) ++pos;
        if (pos < cmd.size()) out.rest = cmd.substr(pos);
        out.valid = true;
        return out;
    }

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
