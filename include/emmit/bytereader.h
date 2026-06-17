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
 * @file bytereader.h
 * @brief Lector secuencial de buffers de bytes con validacion de limites.
 *
 * Proporciona:
 *   - Cursor: marca de posicion guardada dentro de un ByteReader.
 *   - ByteReaderError: excepcion lanzada cuando una lectura excede el buffer.
 *   - ByteReader: lector seguro con auto-incremento de offset, pila de cursores
 *     y creacion de sublectores para leer secciones de tamano conocido.
 *
 * Todas las lecturas multi-byte usan orden little-endian.
 */

#ifndef BYTEREADER_H
#define BYTEREADER_H

#include <cstdint>
#include <stdexcept>
#include <vector>

/**
 * @brief Marca de posicion (offset) guardada dentro de un ByteReader.
 *
 * Se crea con ByteReader::save() y se restaura con ByteReader::restore()
 * o mediante la pila de cursores (push/pop/drop).
 */
struct Cursor {
    uint64_t offset = 0; ///< Offset almacenado en el momento de crear el cursor
};

/**
 * @brief Excepcion lanzada por ByteReader cuando una lectura excede el buffer.
 */
class ByteReaderError : public std::exception {
  public:
    /**
     * @brief Construye el error con el mensaje indicado.
     *
     * @param msg Descripcion del error de lectura.
     */
    explicit ByteReaderError(std::string msg) : message(std::move(msg)) {}

    /**
     * @brief Devuelve el mensaje de error como cadena C.
     *
     * @return Puntero a la cadena de mensaje de error.
     */
    const char *what() const noexcept override { return message.c_str(); }

  private:
    std::string message; ///< Mensaje descriptivo del error
};

/**
 * @brief Lector seguro de buffers de bytes con auto-incremento del offset.
 *
 * Permite leer datos binarios de forma secuencial validando que cada lectura
 * no exceda el tamano del buffer.  Es el complemento natural de ByteWriter.
 *
 * Caracteristicas:
 *   - Lecturas de 8, 16, 32, 40 y 64 bits en little-endian.
 *   - Lectura de bloques arbitrarios de bytes.
 *   - Salto (skip) y posicionamiento absoluto (seek).
 *   - Pila de cursores (push/pop/drop) para backtracking sin acoplamiento
 * externo.
 *   - Creacion de sublectores (subreader) para parsear secciones de tamano
 * conocido.
 */
typedef struct ByteReader {
    /**
     * @brief Pila de cursores guardados.
     *
     * Permite guardar la posicion actual con push() y recuperarla con pop(),
     * o descartarla con drop() sin cambiar el offset.
     */
    std::vector<Cursor> cursor_stack;

    /**
     * @brief Referencia al buffer de entrada (bytecode, datos, ejecutables,
     * etc.).
     */
    const std::vector<uint8_t> &input;

    /**
     * @brief Offset actual de lectura dentro del buffer.
     */
    uint64_t offset = 0;

    /**
     * @brief Construye el lector apuntando al buffer de datos indicado.
     *
     * @param data Referencia constante al vector de bytes de entrada.
     */
    ByteReader(const std::vector<uint8_t> &data) : input(data) {}

    /**
     * @brief Verifica que quedan al menos @p size bytes disponibles desde el
     * offset actual.
     *
     * Lanza ByteReaderError si el buffer no tiene suficientes bytes.
     *
     * @param size Numero minimo de bytes requeridos.
     * @throws ByteReaderError si offset + size > input.size().
     */
    void require(size_t size) const {
        if (offset + size > input.size()) {
            throw ByteReaderError("ByteReader: lectura fuera de rango");
        }
    }

    /**
     * @brief Lee un byte (8 bits) y avanza el offset en 1.
     *
     * @return Byte leido.
     * @throws ByteReaderError si no hay bytes disponibles.
     */
    uint8_t read8() {
        require(1);
        return input[offset++]; // leer byte y avanzar
    }

    /**
     * @brief Lee 2 bytes en little-endian y avanza el offset en 2.
     *
     * @return Valor de 16 bits leido.
     * @throws ByteReaderError si no hay suficientes bytes.
     */
    uint16_t read16() {
        require(2);
        uint16_t v = (uint16_t)input[offset] |           // byte bajo
                     ((uint16_t)input[offset + 1] << 8); // byte alto
        offset += 2;
        return v;
    }

    /**
     * @brief Lee 4 bytes en little-endian y avanza el offset en 4.
     *
     * @return Valor de 32 bits leido.
     * @throws ByteReaderError si no hay suficientes bytes.
     */
    uint32_t read32() {
        require(4);
        uint32_t v = (uint32_t)input[offset] |
                     ((uint32_t)input[offset + 1] << 8) |
                     ((uint32_t)input[offset + 2] << 16) |
                     ((uint32_t)input[offset + 3] << 24);
        offset += 4;
        return v;
    }

    /**
     * @brief Lee 8 bytes en little-endian y avanza el offset en 8.
     *
     * @return Valor de 64 bits leido.
     * @throws ByteReaderError si no hay suficientes bytes.
     */
    uint64_t read64() {
        require(8);
        uint64_t v = (uint64_t)input[offset] |
                     ((uint64_t)input[offset + 1] << 8) |
                     ((uint64_t)input[offset + 2] << 16) |
                     ((uint64_t)input[offset + 3] << 24) |
                     ((uint64_t)input[offset + 4] << 32) |
                     ((uint64_t)input[offset + 5] << 40) |
                     ((uint64_t)input[offset + 6] << 48) |
                     ((uint64_t)input[offset + 7] << 56);
        offset += 8;
        return v;
    }

    /**
     * @brief Lee exactamente 5 bytes (40 bits) en little-endian y avanza el
     * offset en 5.
     *
     * Util para leer campos de 40 bits del formato .velb.
     *
     * @return Valor de 40 bits leido en los 5 bytes bajos de un uint64_t.
     * @throws ByteReaderError si no hay suficientes bytes.
     */
    uint64_t read40() {
        require(5);
        uint64_t v = (uint64_t)input[offset] |
                     ((uint64_t)input[offset + 1] << 8) |
                     ((uint64_t)input[offset + 2] << 16) |
                     ((uint64_t)input[offset + 3] << 24) |
                     ((uint64_t)input[offset + 4] << 32);
        offset += 5;
        return v;
    }

    /**
     * @brief Lee un bloque de @p size bytes y avanza el offset en @p size.
     *
     * @param size Numero de bytes a leer.
     * @return Vector con los bytes leidos.
     * @throws ByteReaderError si no hay suficientes bytes.
     */
    std::vector<uint8_t> read_bytes(size_t size) {
        require(size);
        std::vector<uint8_t> out(input.begin() + offset,
                                 input.begin() + offset + size);
        offset += size;
        return out;
    }

    /**
     * @brief Salta @p size bytes sin leerlos y avanza el offset en @p size.
     *
     * @param size Numero de bytes a saltar.
     * @throws ByteReaderError si no hay suficientes bytes para saltar.
     */
    void skip(size_t size) {
        require(size);
        offset += size;
    }

    /**
     * @brief Indica si se ha llegado al final del buffer.
     *
     * @return true si offset >= input.size() (no quedan bytes por leer).
     */
    bool eof() const { return offset >= input.size(); }

    /**
     * @brief Crea un cursor con el offset actual.
     *
     * @return Cursor apuntando a la posicion actual.
     */
    Cursor save() const { return Cursor{offset}; }

    /**
     * @brief Restaura el offset al valor almacenado en el cursor @p c.
     *
     * @param c Cursor con la posicion a restaurar.
     * @throws ByteReaderError si el offset del cursor esta fuera del buffer.
     */
    void restore(const Cursor &c) {
        if (c.offset > input.size()) {
            throw ByteReaderError("ByteReader: cursor fuera de rango");
        }
        offset = c.offset; // restaurar posicion
    }

    /**
     * @brief Guarda el offset actual en la pila de cursores.
     *
     * Usar pop() para restaurar o drop() para descartar sin restaurar.
     */
    void push() {
        cursor_stack.push_back(save()); // guardar posicion actual en la pila
    }

    /**
     * @brief Restaura el offset al ultimo cursor guardado y lo elimina de la
     * pila.
     *
     * @throws ByteReaderError si la pila esta vacia (pop sin push previo).
     */
    void pop() {
        if (cursor_stack.empty()) {
            throw ByteReaderError("ByteReader: pop sin push");
        }
        restore(cursor_stack.back()); // restaurar cursor del tope de la pila
        cursor_stack.pop_back();      // eliminar cursor de la pila
    }

    /**
     * @brief Descarta el ultimo cursor de la pila sin cambiar el offset actual.
     *
     * Se usa cuando ya no se necesita volver atras pero se quiere limpiar la
     * pila.
     *
     * @throws ByteReaderError si la pila esta vacia.
     */
    void drop() {
        if (cursor_stack.empty()) {
            throw ByteReaderError("ByteReader: drop sin push");
        }
        cursor_stack.pop_back(); // descartar sin restaurar
    }

    /**
     * @brief Posiciona el lector en el offset absoluto indicado.
     *
     * @param new_offset Offset absoluto dentro del buffer al que saltar.
     * @throws ByteReaderError si new_offset esta fuera del buffer.
     */
    void seek(uint64_t new_offset) {
        if (new_offset > input.size()) {
            throw ByteReaderError("ByteReader: seek fuera de rango");
        }
        offset = new_offset; // posicionar en el offset indicado
    }

    /**
     * @brief Crea un sublector que comparte el mismo buffer desde la posicion
     * actual.
     *
     * El sublector empieza en el offset del padre y tiene una ventana de @p
     * size bytes. Usar seek() sobre el hijo para cambiar su posicion de inicio
     * si es necesario.
     *
     * Util para parsear secciones de tamano conocido sin consumir el offset del
     * padre.
     *
     * @param size Numero de bytes de la ventana del sublector.
     * @return Nuevo ByteReader apuntando a la misma posicion del padre.
     * @throws ByteReaderError si no hay @p size bytes disponibles desde el
     * offset actual.
     */
    ByteReader subreader(size_t size) {
        require(size);
        ByteReader r(input);
        r.offset = offset; // el hijo empieza donde el padre
        return r;
    }
} ByteReader;

#endif // BYTEREADER_H
