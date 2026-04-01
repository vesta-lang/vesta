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
#ifndef SYNC_IO_H
#define SYNC_IO_H

#include <mutex>
#include <iostream>
#include <sstream>
#include <string>

namespace vesta {
    /**
     * @brief Mutex global para sincronizar accesos a std::cout / std::cerr entre hilos.
     *
     * Defínelo en un único fichero .cpp (sync_io.cpp). Incluye este header donde
     * necesites imprimir desde varios hilos para evitar salidas mezcladas.
     */
    extern std::mutex cout_mutex;

    void print_threadsafe(const std::string &s);

    void print_threadsafe(std::ostream &out, const std::string &s);

    /**
     * SyncOStream
     *
     * Uso:
     *   vesta::scout() << "hola " << x << std::endl;
     *
     * Construye la cadena fuera del mutex y la escribe atomically al destruirse
     * o cuando recibe std::endl (flush parcial).
     */
    class SyncOStream {
    public:
        explicit SyncOStream(std::ostream &out_stream = std::cout) : out(out_stream) {}

        // Movable, no copiable
        SyncOStream(SyncOStream&&) = default;
        SyncOStream& operator=(SyncOStream&&) = default;
        SyncOStream(const SyncOStream&) = delete;
        SyncOStream& operator=(const SyncOStream&) = delete;

        ~SyncOStream();

        // operador genérico para cualquier tipo imprimible
        template<typename T>
        SyncOStream& operator<<(T&& value) {
            oss << std::forward<T>(value);
            return *this;
        }

        // sobrecarga para manipuladores como std::endl, std::flush, etc.
        SyncOStream& operator<<(std::ostream& (*manip)(std::ostream&));

        // fuerza el volcado inmediato (escribe bajo mutex y deja buffer vacío)
        void flush_now();

    private:
        std::ostringstream oss;
        std::ostream &out;
    };

    // helper para obtener un SyncOStream temporal
    inline SyncOStream scout(std::ostream &out = std::cout) {
        return SyncOStream(out);
    }

}

#endif //SYNC_IO_H
