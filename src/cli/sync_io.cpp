/*
* VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */


/**
 * @file sync_io.cpp
 * @brief Implementacion de las utilidades de E/S sincronizada de VestaVM.
 *
 * Implementa @c vesta::SyncOStream, @c vesta::DebugLogger y las funciones
 * auxiliares de formateo y volcado hexadecimal del namespace @c vesta.
 * Garantiza salida thread-safe mediante @c cout_mutex.
 */
#include "cli/sync_io.h"

namespace vesta {
    // Definicion del mutex global
    std::mutex cout_mutex;

    void print_threadsafe(const std::string &s) {
        std::lock_guard lk(cout_mutex);
        std::cout << s;
    }

    void print_threadsafe(std::ostream &out, const std::string &s) {
        std::lock_guard lk(cout_mutex);
        out << s;
    }

    SyncOStream::~SyncOStream() {
        flush_now();
    }

    void SyncOStream::flush_now() {
        const std::string s = oss.str();
        if (s.empty()) return;
        oss.str({});
        oss.clear();
        print_threadsafe(out, s);
    }
}
