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
#include <type_traits>
#include <iomanip>

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
        explicit SyncOStream(std::ostream &out_stream = std::cout) : out(out_stream) {
        }

        // Movable, no copiable
        SyncOStream(SyncOStream &&) = default;

        SyncOStream &operator=(SyncOStream &&) = default;

        SyncOStream(const SyncOStream &) = delete;

        SyncOStream &operator=(const SyncOStream &) = delete;

        ~SyncOStream();

        // operador genérico para cualquier tipo imprimible
        template<typename T>
        SyncOStream &operator<<(T &&value) {
            oss << std::forward<T>(value);
            return *this;
        }

        // sobrecarga para manipuladores como std::endl, std::flush, etc.
        SyncOStream &operator<<(std::ostream & (*manip)(std::ostream &));

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

    // Helpers de detección
    template<typename T, typename = void>
    struct has_to_string : std::false_type {
    };

    template<typename T>
    struct has_to_string<T, std::void_t<decltype(std::declval<const T &>().to_string())> >
            : std::true_type {
    };

    template<typename T, typename = void>
    struct has_print_ostream : std::false_type {
    };

    template<typename T>
    struct has_print_ostream<T, std::void_t<decltype(std::declval<const T &>().print(std::declval<std::ostream &>()))> >
            : std::true_type {
    };

    // Fallback para tipos que no exponen to_string() ni print(ostream&)
    template<typename T>
    std::string component_to_string_fallback(const T *p) {
        if (!p) return "<null>";
        std::ostringstream ss;
        ss << "ptr=" << p;
        return ss.str();
    }

    // Conversor genérico que intenta to_string(), luego print(ostream&), luego fallback
    template<typename T>
    std::string component_to_string(const T &obj) {
        if constexpr (has_to_string<T>::value) {
            return obj.to_string();
        } else if constexpr (has_print_ostream<T>::value) {
            std::ostringstream ss;
            obj.print(ss);
            return ss.str();
        } else {
            // No hay forma portable de capturar print() que escriba a stdout (printf),
            // así que devolvemos una representación por dirección si procede.
            return component_to_string_fallback(&obj);
        }
    }

    // Si el componente es puntero (p. ej. VM*), sobrecarga:
    template<typename T>
    std::string component_to_string(T *p) {
        if (!p) return "<null>";
        using U = std::remove_pointer_t<T>;
        if constexpr (has_to_string<U>::value) {
            return p->to_string();
        } else if constexpr (has_print_ostream<U>::value) {
            std::ostringstream ss;
            p->print(ss);
            return ss.str();
        } else {
            return component_to_string_fallback(p);
        }
    }

    static std::string hex64(uint64_t v) {
        std::ostringstream ss;
        ss << "0x" << std::hex << std::setw(16) << std::setfill('0') << v << std::dec;
        return ss.str();
    }

}

#endif //SYNC_IO_H
