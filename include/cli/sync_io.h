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
 * @file sync_io.h
 * @brief Utilidades de E/S sincronizada y diagnostico para entornos multihilo.
 *
 * Proporciona:
 *   - Un mutex global (cout_mutex) para serializar escrituras en stdout/stderr.
 *   - La clase SyncOStream: stream con bloqueo atomico al destruirse o al recibir std::endl.
 *   - La funcion helper scout() para obtener un SyncOStream temporal en una expresion.
 *   - Traits has_to_string / has_print_ostream para deteccion de metodos de serializacion.
 *   - component_to_string() para convertir cualquier objeto a cadena de forma generica.
 *   - hex64() / hex8() para formatear enteros como cadenas hexadecimales.
 *   - dump() para volcar bloques de memoria en formato hex legible.
 *   - DebugLogger: logger de diagnostico que escribe en decode_log.txt con dump de memoria.
 *   - fmt() para construir cadenas con varargs mediante fold-expression.
 *   - scout_decode() para acceder al DebugLogger global de decodificacion.
 */

#ifndef SYNC_IO_H
#define SYNC_IO_H

#include <fstream>
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
     * Definido en sync_io.cpp.  Incluir este header en cualquier modulo que necesite
     * imprimir desde varios hilos para evitar salidas mezcladas.
     */
    extern std::mutex cout_mutex;

    /**
     * @brief Imprime la cadena @p s en stdout de forma thread-safe.
     *
     * Adquiere cout_mutex antes de escribir y lo libera al terminar.
     *
     * @param s Cadena a imprimir.
     */
    void print_threadsafe(const std::string &s);

    /**
     * @brief Imprime la cadena @p s en el stream @p out de forma thread-safe.
     *
     * Adquiere cout_mutex antes de escribir y lo libera al terminar.
     *
     * @param out Stream de salida destino.
     * @param s   Cadena a imprimir.
     */
    void print_threadsafe(std::ostream &out, const std::string &s);

    /**
     * @brief Stream sincronizado que acumula texto y lo vuelca atomicamente.
     *
     * Uso tipico:
     * @code
     *   vesta::scout() << "hola " << x << std::endl;
     * @endcode
     *
     * Construye la cadena en un ostringstream privado sin tener el mutex y
     * adquiere cout_mutex solo al destruirse el objeto (o al recibir std::endl
     * o std::flush), garantizando que la salida de cada uso sea contigua.
     */
    class SyncOStream {
    public:
        /**
         * @brief Construye un SyncOStream que vuelca en @p out_stream al destruirse.
         *
         * @param out_stream Stream de salida destino (por defecto std::cout).
         */
        explicit SyncOStream(std::ostream &out_stream = std::cout) : out(out_stream) {}

        /// Movible pero no copiable para garantizar propiedad unica del buffer
        SyncOStream(SyncOStream &&) = default;
        SyncOStream &operator=(SyncOStream &&) = default;
        SyncOStream(const SyncOStream &) = delete;
        SyncOStream &operator=(const SyncOStream &) = delete;

        /**
         * @brief Destructor: vuelca el buffer acumulado en el stream de salida bajo mutex.
         */
        ~SyncOStream();

        /**
         * @brief Operador de insercion generico para cualquier tipo imprimible.
         *
         * Acumula el valor en el buffer interno sin adquirir el mutex.
         *
         * @tparam T Tipo del valor a insertar.
         * @param value Valor a insertar en el buffer.
         * @return Referencia a este SyncOStream para encadenamiento.
         */
        template<typename T>
        SyncOStream &operator<<(T &&value) {
            oss << std::forward<T>(value); // acumular sin bloqueo
            return *this;
        }

        /**
         * @brief Sobrecarga para manipuladores de stream (std::endl, std::flush, etc.).
         *
         * Aplica el manipulador al buffer interno. Si es std::endl o std::flush
         * fuerza un volcado inmediato bajo mutex para que la salida sea visible al instante.
         *
         * @param manip Manipulador de stream.
         * @return Referencia a este SyncOStream para encadenamiento.
         */
        SyncOStream &operator<<(std::ostream & (*manip)(std::ostream &)) {
            manip(oss); // aplicar el manipulador al buffer interno

            // forzar volcado si el manipulador inserta fin de linea
            if (manip == static_cast<std::ostream& (*)(std::ostream &)>(std::endl)) {
                flush_now();
            }
            // forzar volcado si el manipulador es flush explicito
            if (manip == static_cast<std::ostream& (*)(std::ostream &)>(std::flush)) {
                flush_now();
            }
            return *this;
        }

        /**
         * @brief Vuelca inmediatamente el buffer en el stream de salida bajo mutex.
         *
         * Tras el volcado el buffer interno queda vacio.
         */
        void flush_now();

    private:
        std::ostringstream oss; ///< Buffer de acumulacion (sin mutex)
        std::ostream &     out; ///< Stream de salida destino
    };

    /**
     * @brief Crea un SyncOStream temporal para uso en expresiones de una sola linea.
     *
     * @param out Stream de salida destino (por defecto std::cout).
     * @return Nuevo SyncOStream configurado para el stream indicado.
     */
    inline SyncOStream scout(std::ostream &out = std::cout) {
        return SyncOStream(out);
    }

    // ---------------------------------------------------------------------------
    // Traits de deteccion de metodos de serializacion
    // ---------------------------------------------------------------------------

    /**
     * @brief Trait que detecta si el tipo T tiene un metodo to_string() const.
     *
     * @tparam T Tipo a inspeccionar.
     */
    template<typename T, typename = void>
    struct has_to_string : std::false_type {};

    template<typename T>
    struct has_to_string<T, std::void_t<decltype(std::declval<const T &>().to_string())> >
            : std::true_type {};

    /**
     * @brief Trait que detecta si el tipo T tiene un metodo print(std::ostream&) const.
     *
     * @tparam T Tipo a inspeccionar.
     */
    template<typename T, typename = void>
    struct has_print_ostream : std::false_type {};

    template<typename T>
    struct has_print_ostream<T, std::void_t<decltype(std::declval<const T &>().print(std::declval<std::ostream &>()))> >
            : std::true_type {};

    /**
     * @brief Fallback para tipos que no exponen to_string() ni print(ostream&).
     *
     * Devuelve "<null>" si el puntero es nulo, o "ptr=<direccion>" en caso contrario.
     *
     * @tparam T Tipo apuntado.
     * @param p Puntero al objeto.
     * @return Cadena con la representacion por defecto.
     */
    template<typename T>
    std::string component_to_string_fallback(const T *p) {
        if (!p) return "<null>";
        std::ostringstream ss;
        ss << "ptr=" << p;
        return ss.str();
    }

    /**
     * @brief Convierte un objeto a cadena usando to_string(), print(ostream&) o fallback.
     *
     * Seleccion en tiempo de compilacion mediante if constexpr:
     *   1. Si T tiene to_string() const, lo llama.
     *   2. Si T tiene print(ostream&) const, redirige a un ostringstream.
     *   3. En otro caso, usa component_to_string_fallback().
     *
     * @tparam T Tipo del objeto.
     * @param obj Objeto a serializar.
     * @return Cadena con la representacion del objeto.
     */
    template<typename T>
    std::string component_to_string(const T &obj) {
        if constexpr (has_to_string<T>::value) {
            return obj.to_string();
        } else if constexpr (has_print_ostream<T>::value) {
            std::ostringstream ss;
            obj.print(ss);
            return ss.str();
        } else {
            return component_to_string_fallback(&obj);
        }
    }

    /**
     * @brief Sobrecarga de component_to_string para punteros.
     *
     * Maneja el caso nullptr antes de delegar en la version por referencia.
     *
     * @tparam T Tipo apuntado.
     * @param p Puntero al objeto a serializar.
     * @return Cadena con la representacion, o "<null>" si p es nullptr.
     */
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

    /**
     * @brief Formatea un entero de 64 bits como cadena hexadecimal con prefijo "0x" y relleno de ceros.
     *
     * Produce siempre 16 digitos hexadecimales para representar de forma consistente direcciones
     * o valores de 64 bits.
     *
     * @param v Valor a formatear.
     * @return Cadena con el formato "0x<16 digitos hex>".
     */
    static std::string hex64(uint64_t v) {
        std::ostringstream ss;
        ss << "0x" << std::hex << std::setw(16) << std::setfill('0') << v << std::dec;
        return ss.str();
    }

    /**
     * @brief Formatea un byte como cadena hexadecimal de dos digitos en mayusculas.
     *
     * Usa una tabla de consulta estatica para mayor velocidad.
     *
     * @param v Byte a formatear.
     * @return Cadena de dos caracteres con la representacion hexadecimal (p.ej. "FF", "0A").
     */
    static std::string hex8(uint8_t v) {
        static const char *lut = "0123456789ABCDEF"; // tabla de consulta hex
        std::string        s;
        s += lut[(v >> 4) & 0xF]; // nibble alto
        s += lut[v & 0xF];        // nibble bajo
        return s;
    }

    /**
     * @brief Vuelca un bloque de memoria real como cadena hex con grupos de 8 bytes por linea.
     *
     * @param code            Puntero al inicio del bloque de memoria a volcar.
     * @param size_dump_bytes Numero de bytes a incluir en el volcado.
     * @return Cadena con los bytes en hexadecimal separados por espacios y saltos de linea.
     */
    static std::string dump(const uint8_t *code, uint16_t size_dump_bytes) {
        std::string dump_;
        for (int i = 1; i <= size_dump_bytes; i++) {
            uint8_t b = code[i - 1];
            dump_ += hex8(b) + " ";              // byte en hex
            if (((i % 8) == 0)) dump_ += "\n";  // salto de linea cada 8 bytes
        }
        return dump_;
    }

    /**
     * @brief Vuelca un bloque de memoria virtual de la VM como cadena hex.
     *
     * Utiliza el manejador de memoria virtual de la VM para acceder a los bytes
     * en lugar de leer memoria del host directamente.
     *
     * @param vm_mem          Manejador de memoria virtual de la VM.
     * @param v_address       Direccion virtual de inicio dentro de la VM.
     * @param size_dump_bytes Numero de bytes a volcar.
     * @return Cadena con los bytes en hexadecimal separados por espacios y saltos de linea.
     */
    static std::string dump(vm::VirtualMemory vm_mem, uint16_t v_address, uint16_t size_dump_bytes) {
        std::string dump_;
        for (int i = 1; i <= size_dump_bytes; i++) {
            uint8_t b = vm_mem[v_address + i - 1]; // leer byte por direccion virtual
            dump_ += hex8(b) + " ";
            if (((i % 8) == 0)) dump_ += "\n";
        }
        return dump_;
    }

    /**
     * @brief Logger de diagnostico para decodificacion de instrucciones.
     *
     * Escribe en el fichero "decode_log.txt" (modo append).  Ademas de la
     * insercion normal de valores, ofrece dump_memory() para volcar bloques
     * de memoria con formato hex + ASCII en columnas de 16 bytes.
     *
     * Acceder via scout_decode() para obtener la instancia global.
     */
    class DebugLogger {
    public:
        /**
         * @brief Abre el fichero de log en modo append al construirse.
         */
        DebugLogger() {
            file_.open("decode_log.txt", std::ios::app); // abrir en modo append
        }

        /**
         * @brief Cierra el fichero de log al destruirse.
         */
        ~DebugLogger() {
            file_.close();
        }

        /**
         * @brief Operador de insercion generico para cualquier tipo serializable.
         *
         * @tparam T Tipo del valor a escribir.
         * @param v Valor a escribir en el fichero de log.
         * @return Referencia a este logger para encadenamiento.
         */
        template<typename T>
        DebugLogger &operator<<(const T &v) {
            file_ << v;
            return *this;
        }

        /**
         * @brief Sobrecarga para manipuladores de stream (std::endl, std::flush, etc.).
         *
         * @param manip Manipulador a aplicar al fichero de log.
         * @return Referencia a este logger para encadenamiento.
         */
        DebugLogger &operator<<(std::ostream & (*manip)(std::ostream &)) {
            file_ << manip;
            return *this;
        }

        /**
         * @brief Vuelca un bloque de memoria en el log con formato hex de 16 bytes por linea.
         *
         * Imprime la cabecera "[MEMORY DUMP] size=<N> bytes" y despues las filas con
         * offset en hex y los bytes de cada fila.
         *
         * @param ptr  Puntero al inicio del bloque de memoria a volcar.
         * @param size Numero de bytes a incluir en el volcado.
         */
        void dump_memory(const void *ptr, size_t size) {
            const uint8_t *p = static_cast<const uint8_t *>(ptr);

            file_ << "    [MEMORY DUMP] size=" << size << " bytes\n";

            for (size_t i = 0; i < size; i += 16) {
                // imprimir offset de la fila
                file_ << "    " << std::setw(4) << std::setfill('0') << std::hex << i << ": ";

                // imprimir bytes de la fila (hasta 16)
                for (size_t j = 0; j < 16 && i + j < size; ++j) {
                    file_ << std::setw(2) << std::setfill('0') << std::hex
                            << (int) p[i + j] << " ";
                }

                file_ << "\n";
            }

            file_ << std::dec; // restaurar formato decimal
        }

    private:
        std::ofstream file_; ///< Fichero de log en modo append
    };

    /**
     * @brief Construye una cadena concatenando todos los argumentos mediante fold-expression.
     *
     * Equivalente a (oss << arg1 << arg2 << ...).str() pero sin necesidad de crear
     * el ostringstream manualmente.
     *
     * @tparam Args Tipos de los argumentos a concatenar.
     * @param args  Valores a concatenar en la cadena resultante.
     * @return Cadena con todos los argumentos concatenados.
     */
    template<typename... Args>
    std::string fmt(Args&&... args) {
        std::ostringstream oss;
        (oss << ... << args); // fold-expression C++17
        return oss.str();
    }

    /**
     * @brief Devuelve la instancia global del DebugLogger para decodificacion.
     *
     * La instancia es estatica y se inicializa la primera vez que se llama.
     * Escribe en "decode_log.txt" en el directorio de trabajo actual.
     *
     * @return Referencia al DebugLogger global.
     */
    inline DebugLogger &scout_decode() {
        static DebugLogger logger; // instancia estatica (singleton)
        return logger;
    }

} // namespace vesta

#endif // SYNC_IO_H
