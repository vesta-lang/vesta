/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codec.h
 * @brief Cada nodo, a bytes y de vuelta.
 *
 * Aqui es donde cada genero de nodo sabe escribirse y leerse.  El almacen no
 * necesita saber nada de esto: recibe bytes y una huella.
 *
 * **La huella se calcula sobre lo serializado.**  No sobre los campos por
 * separado ni sobre una seleccion de ellos: sobre exactamente los mismos bytes
 * que se van a guardar.  Asi no pueden divergir -- si dos nodos se serializan
 * igual, son el mismo, y si alguien anade un campo, la huella cambia sola sin
 * que haya que acordarse de meterlo tambien en el calculo.  Ese olvido es
 * justo el fallo que deja un cache sirviendo la version antigua de algo que si
 * cambio.
 *
 * Al deserializar nunca se confia en los bytes: pueden venir de un cache de
 * otra version o de un fichero a medias.  El lector avisa (@ref ByteReader::ok)
 * y estas funciones devuelven @c false en vez de un nodo a medio construir.
 */

#ifndef VXDBG_CODEC_H
#define VXDBG_CODEC_H

#include "vxdbg/backend_meta.h"
#include "vxdbg/execution_edge.h"
#include "vxdbg/lowering_map.h"
#include "vxdbg/roots.h"
#include "vxdbg/serialize.h"
#include "vxdbg/source_meta.h"
#include "vxdbg/store.h"

namespace vxdbg {

/// @name Capa semantica
/// @{
StoredNode encode(const FileNode &n);
bool decode(const StoredNode &s, FileNode &out);

/// Toma la entidad RESUELTA y no la almacenable: la cabecera la pone el
/// codec, que es de quien es.  Una @ref LanguageEntity vale igual, por ser
/// una de ellas con cabecera.
StoredNode encode(const ResolvedEntity &n);
bool decode(const StoredNode &s, LanguageEntity &out);

StoredNode encode(const ScopeNode &n);
bool decode(const StoredNode &s, ScopeNode &out);

StoredNode encode(const VariableNode &n);
bool decode(const StoredNode &s, VariableNode &out);

StoredNode encode(const StatementNode &n);
bool decode(const StoredNode &s, StatementNode &out);
/// @}

/// @name Bajada al intermedio
/// @{
StoredNode encode(const LoweringMap &n);
bool decode(const StoredNode &s, LoweringMap &out);
/// @}

/// @name Capa del backend
/// @{
StoredNode encode(const CodeNode &n);
bool decode(const StoredNode &s, CodeNode &out);

StoredNode encode(const CodeDebug &n);
bool decode(const StoredNode &s, CodeDebug &out);

StoredNode encode(const VariableMap &n);
bool decode(const StoredNode &s, VariableMap &out);
/// @}

/// @name Entrada al grafo
/// @{
StoredNode encode(const ArtifactMap &n);
bool decode(const StoredNode &s, ArtifactMap &out);

StoredNode encode(const SpanMap &n);
bool decode(const StoredNode &s, SpanMap &out);
/// @}

/// @name Transferencias de control
/// @{
StoredNode encode(const ExecutionEdge &n);
bool decode(const StoredNode &s, ExecutionEdge &out);

StoredNode encode(const InlineSite &n);
bool decode(const StoredNode &s, InlineSite &out);
/// @}

/**
 * @brief Guarda un nodo cualquiera, sellandolo antes.
 *
 * Recoge la secuencia que hay que seguir siempre -- serializar, sellar,
 * guardar -- para que no haya que repetirla en cada sitio ni quede a criterio
 * de quien llame.
 *
 * @tparam T Genero del nodo.
 * @param store Donde guardarlo.
 * @param node El nodo.
 * @param out_hash Recibe la huella con la que quedo guardado.
 * @return @c true si se guardo (o ya estaba).
 */
template <typename T>
bool store_node(NodeStore &store, const T &node, ContentHash &out_hash) {
    StoredNode s = encode(node);
    out_hash = seal(s);
    return store.put(s);
}

/**
 * @brief Lee un nodo de un genero concreto.
 * @tparam T Genero esperado.
 * @param store De donde leerlo.
 * @param hash Huella.
 * @param out Recibe el nodo.
 * @return @c true si estaba y se pudo interpretar.
 */
template <typename T>
bool load_node(const NodeStore &store, ContentHash hash, T &out) {
    StoredNode s;
    if (!store.get(hash, s)) return false;
    return decode(s, out);
}

} // namespace vxdbg

#endif // VXDBG_CODEC_H
