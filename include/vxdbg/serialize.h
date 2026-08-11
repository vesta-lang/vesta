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
 * Lo minimo para pasar un nodo a bytes y recuperarlo, sin conocer ningun nodo
 * concreto: cada uno se serializa a si mismo llamando a estos primitivos.
 *
 * Los primitivos generales (enteros, cadenas, bytes en crudo, y la promesa de
 * que leer nunca desborda) viven ahora en @c util/serialize.h: tambien los
 * necesita el fichero de hechos del ASA, y ni tenia sentido que el analisis
 * dependiera de la depuracion ni copiar el codigo.  Aqui solo queda lo que es
 * de vxdbg -- la huella de contenido y el identificador de nodo --, y quien lo
 * usaba antes no tiene que cambiar nada.
 */

#ifndef VXDBG_SERIALIZE_H
#define VXDBG_SERIALIZE_H

#include "util/serialize.h"
#include "vxdbg/ids.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vxdbg {

/**
 * @brief Acumula bytes, con los tipos propios de vxdbg encima.
 */
class ByteWriter : public util::ByteWriter {
  public:
    /// Escribe una huella (las dos mitades).
    void hash(const ContentHash &h) {
        u64(h.lo);
        u64(h.hi);
    }

    /// Escribe un identificador de nodo (su huella).
    template <typename Tag> void id(const NodeId<Tag> &v) { hash(v.hash); }
};

/**
 * @brief Lee bytes sin salirse, con los tipos propios de vxdbg encima.
 */
class ByteReader : public util::ByteReader {
  public:
    using util::ByteReader::ByteReader;

    ContentHash hash() {
        ContentHash h;
        h.lo = u64();
        h.hi = u64();
        return h;
    }

    /// Lee un identificador de nodo.
    template <typename Tag> NodeId<Tag> id() { return NodeId<Tag>{hash()}; }
};

} // namespace vxdbg

#endif // VXDBG_SERIALIZE_H
