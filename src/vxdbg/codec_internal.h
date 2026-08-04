/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codec_internal.h
 * @brief Lo que comparten los codecs de los distintos nodos.
 *
 * Privado del subsistema: no vive en @c include/ porque nadie de fuera lo
 * necesita.  Estaba repetido en cada fichero de codec, y dos copias de la misma
 * comprobacion acaban divergiendo justo cuando importa.
 */

#ifndef VXDBG_CODEC_INTERNAL_H
#define VXDBG_CODEC_INTERNAL_H

#include "vxdbg/codec.h"

namespace vxdbg {
namespace codec_detail {

/**
 * @brief Prepara el nodo a guardar tomando el encabezado del propio dato.
 *
 * El genero y la version salen del nodo y no se pasan sueltos: repetirlos en
 * cada llamada era pedir que alguien escribiera uno que no corresponde.
 *
 * @param header Encabezado del nodo.
 * @param w Bytes ya escritos.
 * @return El nodo, sin sellar todavia.
 */
inline StoredNode make(const DebugNodeHeader &header, ByteWriter &w) {
    StoredNode s;
    s.header = header;
    s.header.hash = ContentHash{}; // lo pone seal(), nunca a mano
    s.payload = w.take();
    return s;
}

/**
 * @brief Comprueba genero y version antes de interpretar los bytes.
 *
 * La version esperada se toma del TIPO (@c T::kSchemaVersion), no del objeto
 * destino: pedirsela al objeto obligaba a que llegara ya inicializado con la
 * correcta, y bastaba con pasar uno recien construido de otra forma para que la
 * comprobacion dejara de comprobar nada.
 *
 * Leer un nodo de otra version es tan peligroso como leer uno de otro genero:
 * los campos estan en otro sitio y salen valores que parecen buenos.  Se exige
 * la version EXACTA; el dia que haya que leer las antiguas, sera por un camino
 * escrito para eso y no por descuido.
 *
 * @tparam T Genero del nodo que se espera.
 * @param s Nodo guardado.
 * @param kind Genero esperado.
 * @return @c true si el nodo se puede interpretar como @c T.
 */
template <typename T> bool expect(const StoredNode &s, NodeKind kind) {
    return s.header.kind == kind && s.header.schema_version == T::kSchemaVersion;
}

} // namespace codec_detail
} // namespace vxdbg

#endif // VXDBG_CODEC_INTERNAL_H
