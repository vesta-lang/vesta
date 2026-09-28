/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vxi_io.h
 * @brief Las primitivas con que se escribe y se lee un `.vxi`: enteros en
 *        little-endian, el pozo de cadenas y la lectura de un nombre del pozo.
 *
 * Vivian como `static` dentro de `vxi_format.cpp`, asi que cualquier parte del
 * formato que se quisiera en su propio fichero tenia que copiarlas.  Aqui
 * tienen UN dueno, y el escritor y el lector de cada parte las toman de el.
 */

#ifndef VX_MODULE_VXI_IO_H
#define VX_MODULE_VXI_IO_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vx {
namespace vxi_io {

/**
 * @class StringPoolBuilder
 * @brief El pozo de cadenas de un `.vxi` mientras se escribe, sin repetidos:
 *        cada cadena se anade una vez y su segundo uso devuelve el mismo
 *        desplazamiento.
 */
class StringPoolBuilder {
  public:
    /// @brief Reserva el desplazamiento 0 para la cadena vacia, que hace de
    ///        "sin cadena" en los campos opcionales.
    StringPoolBuilder() { offsets_[""] = 0; }

    /**
     * @brief El desplazamiento de @p s en el pozo; si no estaba, lo anade.
     * @param s La cadena.
     * @return Su desplazamiento.
     */
    uint32_t intern(const std::string &s) {
        if (s.empty()) return 0;
        auto it = offsets_.find(s);
        if (it != offsets_.end()) return it->second;
        const uint32_t off = static_cast<uint32_t>(buf_.size());
        offsets_.emplace(s, off);
        buf_.insert(buf_.end(), s.begin(), s.end());
        return off;
    }

    /// @brief Los bytes del pozo.  @return El buffer.
    const std::vector<uint8_t> &bytes() const noexcept { return buf_; }
    /// @brief El tamano del pozo.  @return Bytes.
    size_t size() const noexcept { return buf_.size(); }

  private:
    std::vector<uint8_t> buf_;
    std::unordered_map<std::string, uint32_t> offsets_;
};

/**
 * @brief Escribe un byte.
 * @param b Buffer.
 * @param v Valor.
 */
inline void write_u8(std::vector<uint8_t> &b, uint8_t v) { b.push_back(v); }

/**
 * @brief Escribe un u16 en little-endian.
 * @param b Buffer.
 * @param v Valor.
 */
inline void write_u16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

/**
 * @brief Escribe un u32 en little-endian.
 * @param b Buffer.
 * @param v Valor.
 */
inline void write_u32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

/**
 * @brief Escribe un u64 en little-endian.
 * @param b Buffer.
 * @param v Valor.
 */
inline void write_u64(std::vector<uint8_t> &b, uint64_t v) {
    write_u32(b, static_cast<uint32_t>(v & 0xFFFFFFFFull));
    write_u32(b, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFull));
}

/**
 * @brief Lee un byte, sin salirse del buffer.
 * @param p   Buffer.
 * @param cap Su tamano.
 * @param off Posicion; avanza lo leido.
 * @param v   Destino.
 * @return Falso si no cabe.
 */
inline bool read_u8(const uint8_t *p, size_t cap, size_t &off, uint8_t &v) {
    if (off + 1 > cap) return false;
    v = p[off];
    off += 1;
    return true;
}

/**
 * @brief Lee un u16 little-endian, sin salirse del buffer.
 * @param p   Buffer.
 * @param cap Su tamano.
 * @param off Posicion; avanza lo leido.
 * @param v   Destino.
 * @return Falso si no cabe.
 */
inline bool read_u16(const uint8_t *p, size_t cap, size_t &off, uint16_t &v) {
    if (off + 2 > cap) return false;
    v = static_cast<uint16_t>(p[off]) |
        (static_cast<uint16_t>(p[off + 1]) << 8);
    off += 2;
    return true;
}

/**
 * @brief Lee un u32 little-endian, sin salirse del buffer.
 * @param p   Buffer.
 * @param cap Su tamano.
 * @param off Posicion; avanza lo leido.
 * @param v   Destino.
 * @return Falso si no cabe.
 */
inline bool read_u32(const uint8_t *p, size_t cap, size_t &off, uint32_t &v) {
    if (off + 4 > cap) return false;
    v = static_cast<uint32_t>(p[off]) |
        (static_cast<uint32_t>(p[off + 1]) << 8) |
        (static_cast<uint32_t>(p[off + 2]) << 16) |
        (static_cast<uint32_t>(p[off + 3]) << 24);
    off += 4;
    return true;
}

/**
 * @brief Lee un u64 little-endian, sin salirse del buffer.
 * @param p   Buffer.
 * @param cap Su tamano.
 * @param off Posicion; avanza lo leido.
 * @param v   Destino.
 * @return Falso si no cabe.
 */
inline bool read_u64(const uint8_t *p, size_t cap, size_t &off, uint64_t &v) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (!read_u32(p, cap, off, lo)) return false;
    if (!read_u32(p, cap, off, hi)) return false;
    v = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    return true;
}

/**
 * @brief Lee una cadena del pozo, sin salirse del buffer.
 * @param data       El fichero.
 * @param size       Su tamano.
 * @param name_off   Desplazamiento dentro del pozo.
 * @param name_len   Longitud.
 * @param pool_start Donde empieza el pozo en el fichero.
 * @param out        Destino.
 * @return Falso si no cabe.
 */
inline bool read_name(const uint8_t *data, size_t size, uint32_t name_off,
                      uint32_t name_len, uint32_t pool_start,
                      std::string &out) {
    const size_t abs = static_cast<size_t>(pool_start) + name_off;
    if (abs + name_len > size) return false;
    out.assign(reinterpret_cast<const char *>(data) + abs, name_len);
    return true;
}

} // namespace vxi_io
} // namespace vx

#endif // VX_MODULE_VXI_IO_H
