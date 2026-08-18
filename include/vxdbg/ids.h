/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ids.h
 * @brief Identidad de los nodos de depuracion y direccionamiento por contenido.
 *
 * Todo el sistema de depuracion es un grafo de nodos pequenos e inmutables:
 * modulo, tipo, funcion, ambito, variable, sentencia, bloque, instruccion.
 * Cada uno tiene un identificador ESTABLE y se referencia por el, nunca por su
 * posicion fisica.  Un desplazamiento cambia porque el enlazador coloque el
 * codigo un poco mas alla; una identidad, no.
 *
 * La identidad se deriva del CONTENIDO del nodo (@ref ContentHash).  De ahi
 * salen dos propiedades que buscamos:
 *
 *  - **Incremental de verdad.**  Si una funcion no cambia, su nodo tiene el
 *    mismo hash, ya esta en el almacen y no hay nada que regenerar ni que
 *    volver a escribir.  Recompilar un modulo de cien funciones donde se toco
 *    una rehace una.
 *  - **Compartido sin duplicar.**  Dos modulos que usan el mismo tipo apuntan
 *    al mismo nodo, no a dos copias que puedan divergir.
 *
 * Los identificadores llevan el genero en el tipo (@ref TypeId, @ref FunctionId
 * ...) para que el compilador impida cruzarlos: pasar un identificador de
 * bloque donde se espera uno de tipo no compila, en vez de dar una consulta
 * silenciosamente vacia.
 */

#ifndef VXDBG_IDS_H
#define VXDBG_IDS_H

#include <cstdint>
#include <functional>
#include <string>

namespace vxdbg {

/**
 * @brief Huella del contenido de un nodo.
 *
 * 128 bits: suficiente para que dos nodos distintos no coincidan por accidente,
 * y la mitad de espacio que una huella de 256 en un grafo con muchas
 * referencias.
 */
struct ContentHash {
    uint64_t lo = 0;
    uint64_t hi = 0;

    /// @return @c true si no identifica a nada.
    bool empty() const { return lo == 0 && hi == 0; }

    bool operator==(const ContentHash &o) const {
        return lo == o.lo && hi == o.hi;
    }
    bool operator!=(const ContentHash &o) const { return !(*this == o); }
    bool operator<(const ContentHash &o) const {
        return hi != o.hi ? hi < o.hi : lo < o.lo;
    }

    /// @return La huella en hexadecimal, para nombrar ficheros y para leerla.
    std::string to_hex() const;

    /**
     * @brief Reconstruye una huella desde su forma hexadecimal.
     * @param hex Cadena de 32 digitos hexadecimales.
     * @return La huella; vacia si @p hex no tiene la forma esperada.
     */
    static ContentHash from_hex(const std::string &hex);
};

/**
 * @brief Calcula la huella de una secuencia de bytes.
 * @param data Bytes a resumir.
 * @param size Cuantos.
 * @return Su huella.
 */
ContentHash hash_bytes(const void *data, size_t size);

/**
 * @brief Mezcla una huella con otra, para componer la de un nodo a partir de
 *        las de sus partes.
 *
 * Un tipo que referencia a otros hereda sus huellas: si cambia uno de los que
 * referencia, cambia el suyo, y eso es justo lo que queremos -- su descripcion
 * ya no es la misma.
 *
 * @param acc Huella acumulada.
 * @param h Huella a mezclar.
 * @return La combinacion.
 */
ContentHash hash_combine(ContentHash acc, ContentHash h);

/**
 * @brief Identificador de un nodo, con su genero marcado en el tipo.
 *
 * @tparam Tag Etiqueta que distingue el genero (ver los alias de abajo).
 */
template <typename Tag> struct NodeId {
    ContentHash hash;

    NodeId() = default;
    explicit NodeId(ContentHash h) : hash(h) {}

    /// @return @c true si no apunta a ningun nodo.
    bool empty() const { return hash.empty(); }

    bool operator==(const NodeId &o) const { return hash == o.hash; }
    bool operator!=(const NodeId &o) const { return hash != o.hash; }
    bool operator<(const NodeId &o) const { return hash < o.hash; }
};

/// Etiquetas de genero.  Solo existen para dar identidad a @ref NodeId.
struct ModuleTag {};
struct TypeTag {};
struct FunctionTag {};
struct ScopeTag {};
struct VariableTag {};
struct StatementTag {};
struct IrFunctionTag {};
struct BlockTag {};
struct IrInstrTag {};
struct CodeTag {};

using ModuleId = NodeId<ModuleTag>;         ///< un modulo
using TypeId = NodeId<TypeTag>;             ///< un tipo del lenguaje
using FunctionId = NodeId<FunctionTag>;     ///< una funcion del lenguaje
using ScopeId = NodeId<ScopeTag>;           ///< un ambito lexico
using VariableId = NodeId<VariableTag>;     ///< una variable
using StatementId = NodeId<StatementTag>;   ///< una sentencia o expresion
using IrFunctionId = NodeId<IrFunctionTag>; ///< una funcion en el intermedio
using BlockId = NodeId<BlockTag>;           ///< un bloque basico
using IrInstrId = NodeId<IrInstrTag>;       ///< una instruccion intermedia
using CodeId = NodeId<CodeTag>;             ///< un tramo de codigo generado
using UnitId = NodeId<struct UnitTag>;      ///< una compilacion concreta
using LanguageEntityId =
    NodeId<struct LanguageEntityTag>;  ///< algo que declaro un frontend
using FileId = NodeId<struct FileTag>; ///< un fichero de codigo

} // namespace vxdbg

namespace std {
/// Permite usar las huellas y los identificadores como clave de un mapa.
template <> struct hash<vxdbg::ContentHash> {
    size_t operator()(const vxdbg::ContentHash &h) const noexcept {
        return static_cast<size_t>(h.lo ^ (h.hi * 0x9E3779B97F4A7C15ull));
    }
};
template <typename Tag> struct hash<vxdbg::NodeId<Tag>> {
    size_t operator()(const vxdbg::NodeId<Tag> &id) const noexcept {
        return hash<vxdbg::ContentHash>{}(id.hash);
    }
};
} // namespace std

#endif // VXDBG_IDS_H
