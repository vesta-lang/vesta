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
#ifndef BYTEREADER_H
#define BYTEREADER_H
#include <cstdint>
#include <vector>

struct Cursor {
    uint64_t offset;
};


/**
 * @brief Lector seguro de buffers de bytes con auto-incremento del offset.
 *
 * Permite leer datos binarios de forma secuencial, validando límites
 * y evitando lecturas fuera de rango. Es el complemento natural de ByteWriter.
 */
typedef struct ByteReader {
    /**
     * Lista de cursores, un cursor es una posicion especifica desde la que podemos leer, un offset
     * que hemos decidido guardar.
     */
    std::vector<Cursor> cursor_stack;

    /**
     * @brief Buffer de entrada (bytecode, datos, ejecutables, etc.)
     */
    const std::vector<uint8_t> &input;

    /**
     * @brief Offset actual dentro del buffer.
     */
    uint64_t offset = 0;

    /**
     * @brief Constructor
     * @param data referencia al buffer de entrada
     */
    ByteReader(const std::vector<uint8_t> &data)
        : input(data) {
    }

    /**
     * @brief Verifica si quedan al menos `size` bytes disponibles.
     */
    void require(size_t size) const {
        if (offset + size > input.size()) {
            throw std::runtime_error("ByteReader: lectura fuera de rango");
        }
    }

    /**
     * @brief Lee un byte (8 bits)
     */
    uint8_t read8() {
        require(1);
        return input[offset++];
    }

    /**
     * @brief Lee 16 bits (little endian)
     */
    uint16_t read16() {
        require(2);
        uint16_t v =
                (uint16_t) input[offset] |
                ((uint16_t) input[offset + 1] << 8);
        offset += 2;
        return v;
    }

    /**
     * @brief Lee 32 bits (little endian)
     */
    uint32_t read32() {
        require(4);
        uint32_t v =
                (uint32_t) input[offset] |
                ((uint32_t) input[offset + 1] << 8) |
                ((uint32_t) input[offset + 2] << 16) |
                ((uint32_t) input[offset + 3] << 24);
        offset += 4;
        return v;
    }

    /**
     * @brief Lee 64 bits (little endian)
     */
    uint64_t read64() {
        require(8);
        uint64_t v =
                (uint64_t) input[offset] |
                ((uint64_t) input[offset + 1] << 8) |
                ((uint64_t) input[offset + 2] << 16) |
                ((uint64_t) input[offset + 3] << 24) |
                ((uint64_t) input[offset + 4] << 32) |
                ((uint64_t) input[offset + 5] << 40) |
                ((uint64_t) input[offset + 6] << 48) |
                ((uint64_t) input[offset + 7] << 56);
        offset += 8;
        return v;
    }

    /**
     * @brief Lee exactamente 5 bytes (40 bits)
     */
    uint64_t read40() {
        require(5);
        uint64_t v =
                (uint64_t) input[offset] |
                ((uint64_t) input[offset + 1] << 8) |
                ((uint64_t) input[offset + 2] << 16) |
                ((uint64_t) input[offset + 3] << 24) |
                ((uint64_t) input[offset + 4] << 32);
        offset += 5;
        return v;
    }

    /**
     * @brief Lee un bloque arbitrario de bytes
     */
    std::vector<uint8_t> read_bytes(size_t size) {
        require(size);
        std::vector<uint8_t> out(input.begin() + offset,
                                 input.begin() + offset + size);
        offset += size;
        return out;
    }

    /**
     * @brief Salta N bytes sin leerlos
     */
    void skip(size_t size) {
        require(size);
        offset += size;
    }

    /**
     * @brief Devuelve true si ya no quedan más bytes por leer
     */
    bool eof() const {
        return offset >= input.size();
    }

    /**
     * Obtener un nuevo cursor de la posicion actual
     * @return nuevo cursor.
     */
    Cursor save() const {
        return Cursor{offset};
    }

    /**
     * Restaurar un cursor
     * @param c cursor con la posicion a la que queremos restaurar.
     */
    void restore(const Cursor &c) {
        if (c.offset > input.size()) {
            throw std::runtime_error("ByteReader: cursor fuera de rango");
        }
        offset = c.offset;
    }

    /**
     * guarda el cursor actual en la pila de cursores.
     */
    void push() {
        cursor_stack.push_back(save());
    }

    /**
     * vuelve al ultimo cursor, sacamos un cursor de la pila de cursores.
     */
    void pop() {
        if (cursor_stack.empty()) {
            throw std::runtime_error("ByteReader: pop sin push");
        }
        restore(cursor_stack.back());
        cursor_stack.pop_back();
    }

    /**
     * descarta el cursor sin restaurar, de la pila
     */
    void drop() {
        if (cursor_stack.empty()) {
            throw std::runtime_error("ByteReader: drop sin push");
        }
        cursor_stack.pop_back();
    }

    /**
     * un metodo para saltar a un offset arbitrario
     * @param new_offset offset al que quremos saltar
     */
    void seek(uint64_t new_offset) {
        if (new_offset > input.size()) {
            throw std::runtime_error("ByteReader: seek fuera de rango");
        }
        offset = new_offset;
    }

    /**
     * metodo para crear un sublector. Esta es funciona bien para leer secciones y datos de los que
     * se conocen su tamaño, como las secciones o los espacios de direcciones en el bytecode
     * @param size tamaño del subselecto, si no se conoce no se puede crear un objeto de este tipo
     * @return subselector de lectura
     */
    ByteReader subreader(size_t size) {
        require(size);
        ByteReader r(input);
        r.offset = offset;
        offset += size;
        return r;
    }
} ByteReader;


#endif //BYTEREADER_H
