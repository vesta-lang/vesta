/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file document_store.cpp
 * @brief Implementacion del almacen de documentos y la conversion de
 * posiciones byte->UTF-16.
 */

#include "lsp/document_store.h"

namespace lsp {

uint32_t byte_column_to_utf16(const std::string &line_text,
                              uint32_t byte_column_1based) {
    // La columna 1-based en bytes apunta al byte donde empieza el span; el
    // offset 0-based correspondiente es uno menos.  Una columna 0 o 1 cae
    // al inicio de la linea (caracter 0).
    if (byte_column_1based <= 1) return 0;
    size_t target_byte = static_cast<size_t>(byte_column_1based - 1);
    // No leer mas alla del final de la linea.
    if (target_byte > line_text.size()) target_byte = line_text.size();

    uint32_t utf16_units = 0; // unidades UTF-16 acumuladas hasta target_byte.
    size_t i = 0;             // indice de byte actual dentro de line_text.
    const size_t n = line_text.size();
    while (i < target_byte && i < n) {
        const unsigned char b = static_cast<unsigned char>(line_text[i]);
        // Determinar la longitud en bytes de la secuencia UTF-8 actual segun
        // el byte lider.  Secuencias invalidas se tratan como 1 byte.
        size_t seq_len = 1;
        uint32_t code_point = b;
        if (b < 0x80) {
            // ASCII: 1 byte, 1 unidad UTF-16.
            seq_len = 1;
        } else if ((b & 0xE0) == 0xC0) {
            seq_len = 2;
            code_point = b & 0x1F;
        } else if ((b & 0xF0) == 0xE0) {
            seq_len = 3;
            code_point = b & 0x0F;
        } else if ((b & 0xF8) == 0xF0) {
            seq_len = 4;
            code_point = b & 0x07;
        } else {
            // Byte de continuacion suelto o invalido: avanzar 1 byte como
            // 1 unidad para no quedarse atascado.
            seq_len = 1;
        }

        // Verificar que la secuencia completa cabe; si no, degradar a 1 byte.
        if (seq_len > 1) {
            if (i + seq_len > n) {
                seq_len = 1;
            } else {
                bool valid = true;
                for (size_t k = 1; k < seq_len; ++k) {
                    const unsigned char cb =
                        static_cast<unsigned char>(line_text[i + k]);
                    if ((cb & 0xC0) != 0x80) {
                        valid = false;
                        break;
                    }
                    code_point = (code_point << 6) | (cb & 0x3F);
                }
                if (!valid) seq_len = 1;
            }
        }

        // Contar unidades UTF-16 del code point: 2 si es plano astral
        // (>= U+10000, par surrogate), 1 en otro caso.  Si la secuencia
        // empieza despues de target_byte la habriamos cortado antes; aqui
        // solo contamos secuencias que arrancan dentro del rango.
        if (seq_len == 4 && code_point >= 0x10000) {
            utf16_units += 2;
        } else {
            utf16_units += 1;
        }

        i += seq_len;
    }
    return utf16_units;
}

uint32_t lsp_position_to_byte_offset(const std::string &text, uint32_t line,
                                     uint32_t character) {
    const size_t n = text.size();
    // 1) Localizar el inicio de la linea pedida contando saltos de linea.
    size_t line_start = 0; // offset de byte donde empieza la linea actual.
    uint32_t cur = 0;      // numero de linea actual (0-based).
    size_t i = 0;
    while (i < n && cur < line) {
        if (text[i] == '\n') {
            ++cur;
            line_start = i + 1;
        }
        ++i;
    }
    // Si la linea pedida esta mas alla del final, devolver el final del texto.
    if (cur < line) return static_cast<uint32_t>(n);

    // 2) Avanzar dentro de la linea decodificando UTF-8 hasta acumular
    //    @p character unidades UTF-16 (o hasta el fin de la linea).
    size_t j = line_start;   // offset de byte dentro de la linea.
    uint32_t utf16_seen = 0; // unidades UTF-16 ya consumidas.
    while (j < n && text[j] != '\n' && utf16_seen < character) {
        const unsigned char b = static_cast<unsigned char>(text[j]);
        // Longitud de la secuencia UTF-8 segun el byte lider.
        size_t seq = 1;
        uint32_t cp = b;
        if (b < 0x80) {
            seq = 1;
        } else if ((b & 0xE0) == 0xC0) {
            seq = 2;
            cp = b & 0x1F;
        } else if ((b & 0xF0) == 0xE0) {
            seq = 3;
            cp = b & 0x0F;
        } else if ((b & 0xF8) == 0xF0) {
            seq = 4;
            cp = b & 0x07;
        } else {
            seq = 1; // byte invalido: tratar como 1 unidad.
        }
        // Validar las continuaciones; si no caben o son invalidas, degradar.
        if (seq > 1) {
            if (j + seq > n) {
                seq = 1;
            } else {
                bool ok = true;
                for (size_t k = 1; k < seq; ++k) {
                    const unsigned char cb =
                        static_cast<unsigned char>(text[j + k]);
                    if ((cb & 0xC0) != 0x80) {
                        ok = false;
                        break;
                    }
                    cp = (cp << 6) | (cb & 0x3F);
                }
                if (!ok) seq = 1;
            }
        }
        // Unidades UTF-16 del code point: 2 si es plano astral.
        const uint32_t units = (seq == 4 && cp >= 0x10000) ? 2u : 1u;
        utf16_seen += units;
        j += seq;
    }
    return static_cast<uint32_t>(j);
}

void byte_offset_to_lsp_position(const std::string &text, size_t byte_offset,
                                 uint32_t &out_line, uint32_t &out_char) {
    const size_t n = text.size();
    if (byte_offset > n) byte_offset = n;
    uint32_t line = 0; // linea 0-based.
    uint32_t col = 0;  // caracter 0-based en UTF-16.
    size_t i = 0;
    while (i < byte_offset && i < n) {
        const unsigned char b = static_cast<unsigned char>(text[i]);
        // Longitud de la secuencia UTF-8 segun el byte lider.
        size_t seq = 1;
        uint32_t cp = b;
        if (b < 0x80) {
            seq = 1;
        } else if ((b & 0xE0) == 0xC0) {
            seq = 2;
            cp = b & 0x1F;
        } else if ((b & 0xF0) == 0xE0) {
            seq = 3;
            cp = b & 0x0F;
        } else if ((b & 0xF8) == 0xF0) {
            seq = 4;
            cp = b & 0x07;
        } else {
            seq = 1;
        }
        if (seq > 1) {
            if (i + seq > n) {
                seq = 1;
            } else {
                bool ok = true;
                for (size_t k = 1; k < seq; ++k) {
                    const unsigned char cb =
                        static_cast<unsigned char>(text[i + k]);
                    if ((cb & 0xC0) != 0x80) {
                        ok = false;
                        break;
                    }
                    cp = (cp << 6) | (cb & 0x3F);
                }
                if (!ok) seq = 1;
            }
        }
        if (seq == 1 && b == '\n') {
            ++line;
            col = 0;
        } else {
            col += (seq == 4 && cp >= 0x10000) ? 2u : 1u;
        }
        i += seq;
    }
    out_line = line;
    out_char = col;
}

void DocumentStore::open(const std::string &uri, std::string text) {
    // open y update comparten semantica (full sync): sobreescribir.
    docs_[uri] = std::move(text);
}

void DocumentStore::update(const std::string &uri, std::string text) {
    docs_[uri] = std::move(text);
}

void DocumentStore::close(const std::string &uri) {
    docs_.erase(uri);
}

bool DocumentStore::has(const std::string &uri) const {
    return docs_.find(uri) != docs_.end();
}

const std::string &DocumentStore::text(const std::string &uri) const {
    // Cadena vacia persistente para devolver por referencia cuando falta.
    static const std::string kEmpty;
    auto it = docs_.find(uri);
    return it == docs_.end() ? kEmpty : it->second;
}

std::string DocumentStore::line(const std::string &uri,
                                uint32_t line_0based) const {
    auto it = docs_.find(uri);
    if (it == docs_.end()) return std::string();
    const std::string &txt = it->second;
    // Recorrer el texto contando saltos de linea hasta llegar a la linea
    // pedida; luego copiar hasta el siguiente salto.
    uint32_t cur = 0; // numero de linea actual.
    size_t start = 0; // inicio de la linea actual.
    const size_t n = txt.size();
    for (size_t i = 0; i <= n; ++i) {
        if (i == n || txt[i] == '\n') {
            if (cur == line_0based) {
                size_t end = i;
                // Recortar un posible CR final (CRLF) para no incluirlo.
                if (end > start && txt[end - 1] == '\r') --end;
                return txt.substr(start, end - start);
            }
            ++cur;
            start = i + 1;
        }
    }
    // Linea fuera de rango.
    return std::string();
}

} // namespace lsp
