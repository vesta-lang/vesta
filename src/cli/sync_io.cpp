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

    // Destructor: escribe lo acumulado bajo mutex
    SyncOStream::~SyncOStream() {
        std::lock_guard lk(cout_mutex);
        out << oss.str();
        // asegurar flush del stream de salida
        out.flush();
    }

    // Manejo de manipuladores (std::endl, std::flush, etc.)
    SyncOStream &SyncOStream::operator<<(std::ostream & (*manip)(std::ostream &)) {
        // Aplicar el manipulador al buffer interno
        manip(oss);

        // Si es std::endl (inserta '\n' y flush), hacemos flush_now para que
        // la salida se escriba inmediatamente bajo el mutex.
        // Comparar punteros de función con std::endl es válido para manipuladores
        if (manip == static_cast<std::ostream& (*)(std::ostream &)>(std::endl)) {
            flush_now();
        }
        // Para std::flush también podemos forzar flush_now, lo hago?
        if (manip == static_cast<std::ostream& (*)(std::ostream &)>(std::flush)) {
            flush_now();
        }
        return *this;
    }

    void SyncOStream::flush_now() {
        std::lock_guard<std::mutex> lk(cout_mutex);
        out << oss.str();
        out.flush();
        // vaciar el buffer interno
        oss.str("");
        oss.clear();
    }
}
