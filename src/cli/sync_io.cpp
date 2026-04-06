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


#include "cli/sync_io.h"

namespace vesta {
    // Definición del mutex global
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
