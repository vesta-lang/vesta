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

#ifndef BYTEWRITER_H
#define BYTEWRITER_H
#include <vector>
#include <cstdint>
#include <stdexcept>

/**
 * Permite un controlo de emision de bytes mas seguro con auto-incremento del offset
 */
typedef struct ByteWriter {
    /**
     * Buffer de bytes emitidos, normalmente aqui ira codigo o datos
     */
    std::vector<uint8_t> output;

    /**
     * Offset actual
     */
    uint64_t offset = 0;

    /**
     * Emitir un byte
     * @param value byte a emitir
     */
    void emit8(uint8_t value) {
        output.push_back(value);
        offset += 1;
    }

    /**
     * Emitir 16 bits (little endian)
     * @param value valor de 16 bits
     */
    void emit16(uint16_t value) {
        emit_bytes(&value, sizeof(value));
    }

    /**
     * Emitir 32 bits (little endian)
     * @param value valor de 32bits
     */
    void emit32(uint32_t value) {
        emit_bytes(&value, sizeof(value));
    }

    /**
     * Emitir 64 bits (little endian)
     * @param value valor de 64 bits
     */
    void emit64(uint64_t value) {
        emit_bytes(&value, sizeof(value));
    }

    /**
     * emitir una cantidad arbitraria de bytes
     * @param data buffer de bytes a emitir
     * @param size cantidad de bytes a emitir de este buffer.
     */
    void emit_bytes(const void *data, size_t size) {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(data);
        output.insert(output.end(), p, p + size);
        offset += size;
    }


    /**
     * Emitir solo 5 bytes usando un valor de 8 bytes
     * @param value valor de 5 bytes
     */
    void emit40(uint64_t value) {
        // escribe 5 bytes (40 bits)
        emit8((value >> 0) & 0xFF);
        emit8((value >> 8) & 0xFF);
        emit8((value >> 16) & 0xFF);
        emit8((value >> 24) & 0xFF);
        emit8((value >> 32) & 0xFF);
    }

    /**
     * Se usa para parchear bytecode o datos ya escritos donde se conoce
     * la posicion del dato
     * @param value valor del dato nuevo
     * @param pos posicion para el dato
     */
    void write64_at(uint64_t value, uint64_t pos) {
        if (pos + 8 > output.size()) {
            throw std::runtime_error("write64_at fuera de rango");
        }

        output[pos + 0] = (uint8_t) (value & 0xFF);
        output[pos + 1] = (uint8_t) ((value >> 8) & 0xFF);
        output[pos + 2] = (uint8_t) ((value >> 16) & 0xFF);
        output[pos + 3] = (uint8_t) ((value >> 24) & 0xFF);
        output[pos + 4] = (uint8_t) ((value >> 32) & 0xFF);
        output[pos + 5] = (uint8_t) ((value >> 40) & 0xFF);
        output[pos + 6] = (uint8_t) ((value >> 48) & 0xFF);
        output[pos + 7] = (uint8_t) ((value >> 56) & 0xFF);
    }


} ByteWriter;


#endif //BYTEWRITER_H
