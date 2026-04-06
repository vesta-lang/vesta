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

#include "arena/VirtualMemory.h"

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
        SyncOStream &operator<<(std::ostream & (*manip)(std::ostream &)) {
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

    static std::string hex8(uint8_t v) {
        static const char *lut = "0123456789ABCDEF";
        std::string s;
        s += lut[(v >> 4) & 0xF];
        s += lut[v & 0xF];
        return s;
    }

    /**
     * Se usa para dumpear memoria real
     * @param code codigo o memoria a dumpear
     * @param size_dump_bytes cantidad de bytes a dumpear
     * @return strings dump
     */
    static std::string dump(const uint8_t *code, uint16_t size_dump_bytes) {
        std::string dump_;
        for (int i = 1; i <= size_dump_bytes; i++) {
            uint8_t b = code[i - 1];
            dump_ += hex8(b) + " ";
            if (((i % 8) == 0)) dump_ += "\n";
        }
        return dump_;
    }

    /**
     * Se usa para dumpear memoria de la VM a traves de una direccion virtual
     * @param vm_mem manejador de memoria virtual que usar para dumpear memoria.
     * @param v_address direccion virtual a dumpear
     * @param size_dump_bytes cantidad de bytes a dumpear
     * @return strings dump
     */
    static std::string dump(vm::VirtualMemory vm_mem, uint16_t v_address, uint16_t size_dump_bytes) {
        std::string dump_;
        for (int i = 1; i <= size_dump_bytes; i++) {
            uint8_t b = vm_mem[v_address + i - 1];
            dump_ += hex8(b) + " ";
            if (((i % 8) == 0)) dump_ += "\n";
        }
        return dump_;
    }
}

#endif //SYNC_IO_H
