/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file serialize.h
 * @brief Escribir y leer nodos como bytes.
 *
 * Lo minimo para pasar un nodo a bytes y recuperarlo, sin dependencias y sin
 * conocer ningun nodo concreto: cada uno se serializa a si mismo llamando a
 * estos primitivos.
 *
 * **Leer nunca desborda.**  Los bytes pueden venir de un fichero truncado, de
 * un cache de otra version o de un disco con un sector malo, asi que el lector
 * comprueba el limite en CADA lectura y, al primer fallo, se marca como roto y
 * devuelve ceros en vez de seguir leyendo memoria ajena.  Quien lee comprueba
 * @ref ByteReader::ok una vez al final en lugar de en cada campo, que es lo que
 * hace que el codigo de deserializacion se pueda leer de corrido.
 *
 * Todo en little-endian explicito: el mismo cache tiene que servir en maquinas
 * distintas, y depender del orden nativo lo habria atado a la que lo escribio.
 * Por eso mismo NUNCA se escribe una estructura entera de golpe
 * (`raw(&algo, sizeof(algo))`): eso arrastra el relleno que meta el compilador,
 * el orden de bytes de la maquina y el orden de los campos, y basta con
 * recompilar con otras opciones para que el cache deje de leerse.  Campo a
 * campo, siempre.
 */

#ifndef VXDBG_SERIALIZE_H
#define VXDBG_SERIALIZE_H

#include "vxdbg/ids.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vxdbg {

/**
 * @brief Acumula bytes.
 */
class ByteWriter {
  public:
    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void i64(int64_t v);
    void f64(double v);
    void boolean(bool v);

    /// Escribe una cadena como longitud + contenido.
    void str(const std::string &s);

    /// Escribe una huella (las dos mitades).
    void hash(const ContentHash &h);

    /// Escribe un identificador de nodo (su huella).
    template <typename Tag> void id(const NodeId<Tag> &v) { hash(v.hash); }

    /// Escribe bytes en crudo, sin longitud.
    void raw(const void *data, size_t size);

    /// @return Los bytes acumulados.
    const std::vector<uint8_t> &bytes() const { return buf_; }

    /**
     * @brief Se lleva los bytes y deja el escritor vacio y reutilizable.
     *
     * El vaciado es EXPLICITO: un contenedor del que se han movido los datos
     * queda en un estado valido pero no especificado, asi que dar por hecho
     * que queda vacio es confiar en como esta hecha la biblioteca y no en lo
     * que promete.
     *
     * @return Los bytes acumulados.
     */
    std::vector<uint8_t> take() {
        std::vector<uint8_t> out;
        out.swap(buf_);
        return out;
    }

    /**
     * @brief Reserva sitio de antemano.
     *
     * Quien sepa aproximadamente cuanto ocupa lo que va a escribir se ahorra
     * varias reservas al ir creciendo.  No hace falta llamarlo.
     *
     * @param n Bytes que se esperan.
     */
    void reserve(size_t n) { buf_.reserve(n); }

    /// @return Cuantos bytes lleva.
    size_t size() const { return buf_.size(); }

  private:
    std::vector<uint8_t> buf_;
};

/**
 * @brief Lee bytes, sin salirse nunca.
 */
class ByteReader {
  public:
    ByteReader(const uint8_t *data, size_t size) : data_(data), size_(size) {}
    explicit ByteReader(const std::vector<uint8_t> &v)
        : data_(v.data()), size_(v.size()) {}

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int64_t i64();
    double f64();
    bool boolean();
    std::string str();
    ContentHash hash();

    /// Lee un identificador de nodo.
    template <typename Tag> NodeId<Tag> id() { return NodeId<Tag>{hash()}; }

    /**
     * @brief Lee bytes en crudo.
     * @param out Destino.
     * @param size Cuantos.
     * @return @c true si habia suficientes.
     */
    bool raw(void *out, size_t size);

    /// @return @c true si no ha habido ninguna lectura fallida.
    bool ok() const { return ok_; }

    /**
     * @brief Mira el siguiente byte sin consumirlo.
     *
     * Util cuando hay que decidir que leer segun lo que venga.
     *
     * @return El byte, o 0 si no queda ninguno (sin marcar el lector roto:
     *         ojear el final no es un fallo de lectura).
     */
    uint8_t peek_u8() const {
        return (ok_ && pos_ < size_) ? data_[pos_] : 0u;
    }

    /// @return En que byte va la lectura.  Sirve para decir donde fallo.
    size_t position() const { return pos_; }

    /// @return Cuantos bytes quedan por leer.
    size_t remaining() const { return ok_ ? size_ - pos_ : 0; }

  private:
    /**
     * @brief Comprueba que quedan @p n bytes; si no, marca el lector roto.
     * @param n Bytes que se van a leer.
     * @return @c true si se puede leer.
     */
    bool want(size_t n);

    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace vxdbg

#endif // VXDBG_SERIALIZE_H
