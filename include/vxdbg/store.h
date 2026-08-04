/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file store.h
 * @brief Donde viven los nodos: almacen direccionado por contenido.
 *
 * Guarda objetos pequenos e inmutables bajo su propia huella.  De ahi salen las
 * dos propiedades que se buscan:
 *
 *  - **Incremental de verdad.**  Guardar algo que ya esta es no hacer nada.  Si
 *    una funcion no cambia, su huella es la misma, ya esta guardada, y
 *    recompilar un modulo de cien funciones donde se toco una rehace una.
 *  - **Compartido sin duplicar.**  Dos modulos que usan el mismo tipo apuntan al
 *    mismo objeto, no a dos copias que puedan acabar discrepando.
 *
 * **Nunca un fichero monolitico.**  Un unico fichero con todo habria que
 * rehacerlo entero porque cambiase una linea en cualquier sitio, y habria que
 * cargarlo entero para explicar un fallo que ocurre en un solo punto.
 *
 * El almacen no sabe que hay dentro de un nodo: guarda bytes bajo una huella,
 * con el genero delante para que quien lea sepa como interpretarlos.  Es lo que
 * permite anadir un genero nuevo sin tocarlo.
 */

#ifndef VXDBG_STORE_H
#define VXDBG_STORE_H

#include "vxdbg/ids.h"
#include "vxdbg/node.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vxdbg {

/**
 * @brief Un objeto tal como esta en el almacen: encabezado y contenido.
 *
 * Es LITERALMENTE lo que hay en el fichero.  El genero y la version viven solo
 * en el encabezado y no repetidos fuera: con dos copias, tarde o temprano una
 * dice una cosa y la otra dice otra, y entonces hay que decidir cual manda.
 */
struct StoredNode {
    DebugNodeHeader header;
    std::vector<uint8_t> payload;
};

/**
 * @brief Donde se guardan y de donde se leen los nodos.
 *
 * Interfaz y no una clase concreta: hoy los objetos viven en ficheros cacheados
 * junto a los demas artefactos del modulo, manana pueden estar en memoria
 * durante una sesion interactiva o en un almacen compartido por un equipo.
 * Quien los consulta no deberia enterarse.
 */
class NodeStore {
  public:
    virtual ~NodeStore() = default;

    /**
     * @brief Guarda un nodo bajo su huella.
     *
     * Si ya estaba, no hace nada: dos objetos con la misma huella tienen el
     * mismo contenido, asi que reescribirlo seria trabajo tirado.  Ese "no
     * hacer nada" es justo lo que hace el sistema incremental.
     *
     * La clave sale del propio nodo (@c header.hash) en vez de pasarse aparte:
     * con dos argumentos nada impedia guardar un nodo bajo la huella de otro, y
     * en un almacen donde el nombre ES el contenido eso deja basura que luego
     * pasa por buena.
     *
     * @param node El nodo, con su huella en el encabezado.
     * @return @c true si se pudo guardar (o ya estaba).
     */
    virtual bool put(const StoredNode &node) = 0;

    /**
     * @brief Lee un nodo.
     * @param hash Huella.
     * @param out Recibe el nodo.
     * @return @c true si estaba y se pudo leer.
     */
    virtual bool get(ContentHash hash, StoredNode &out) const = 0;

    /**
     * @brief Si un nodo ya esta guardado.
     *
     * Se pregunta ANTES de construirlo: si la huella se puede calcular sin
     * montar el objeto entero, comprobarlo evita hacer el trabajo.
     *
     * @param hash Huella.
     * @return @c true si esta.
     */
    virtual bool contains(ContentHash hash) const = 0;

    /* No hay `size()`.  El de disco tendria que recorrerlo entero para
     * contarlos, asi que devolveria un numero inventado o un cero que en
     * realidad significa "no lo se" -- y un cero que no quiere decir cero es
     * justo la clase de ambiguedad que acaba en un fallo raro.  Quien pueda
     * contarlos lo ofrece por su cuenta.
     *
     * Tampoco hay `erase()`.  Los nodos son inmutables y su nombre es su
     * contenido: decidir cuando sobra uno es cosa de quien limpie el cache,
     * mirando quien lo referencia, no de quien lo consulta. */
};

/**
 * @brief Almacen en memoria.
 *
 * Para una sesion que no quiere tocar el disco, y para las pruebas.  Se
 * comporta igual que el de disco, que es lo que permite probar contra este y
 * confiar en aquel.
 */
class MemoryNodeStore : public NodeStore {
  public:
    bool put(const StoredNode &node) override;
    bool get(ContentHash hash, StoredNode &out) const override;
    bool contains(ContentHash hash) const override;

    /// @return Cuantos nodos hay.  Solo este almacen lo ofrece: contarlos en
    ///         disco exigiria recorrerlo entero.
    size_t size() const { return nodes_.size(); }

    /// Vacia el almacen.
    void clear() { nodes_.clear(); }

  private:
    std::unordered_map<ContentHash, StoredNode> nodes_;
};

/**
 * @brief Almacen en disco, un fichero por nodo.
 *
 * Los objetos se reparten en subcarpetas por los primeros digitos de su huella.
 * Con decenas de miles de nodos, un solo directorio se vuelve lento de recorrer
 * en casi cualquier sistema de ficheros; repartirlos lo evita sin necesitar un
 * indice que mantener.
 *
 *     <raiz>/ab/abcdef0123456789...
 *
 * Cada fichero lleva delante un encabezado con el genero y la version del
 * esquema, para que un objeto suelto siga siendo interpretable fuera de su
 * sitio.
 */
class FileNodeStore : public NodeStore {
  public:
    /**
     * @param root Carpeta raiz del almacen.
     */
    explicit FileNodeStore(std::string root) : root_(std::move(root)) {}

    bool put(const StoredNode &node) override;
    bool get(ContentHash hash, StoredNode &out) const override;
    bool contains(ContentHash hash) const override;

    /**
     * @brief Ruta del fichero de un nodo.
     * @param hash Huella.
     * @return La ruta completa.
     */
    std::string path_for(ContentHash hash) const;

  private:
    std::string root_;
};

/// Magic del fichero de un nodo: "VXDN" en little-endian.
static constexpr uint32_t VXDBG_NODE_MAGIC = 0x4E445856u;

} // namespace vxdbg

#endif // VXDBG_STORE_H
