/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file store.cpp
 * @brief Implementacion del almacen direccionado por contenido.
 */

#include "vxdbg/store.h"

#include "vxdbg/serialize.h"

#include <filesystem>
#include <fstream>

namespace vxdbg {

// ---------------------------------------------------------------------------
//  En memoria
// ---------------------------------------------------------------------------

bool MemoryNodeStore::put(const StoredNode &node) {
    const ContentHash hash = node.header.hash;
    if (hash.empty()) return false; // un nodo sin huella no tiene donde ir
    auto it = nodes_.find(hash);
    if (it != nodes_.end()) {
        // Ya esta: no se toca.  Ese "no hacer nada" es lo que hace incremental
        // al sistema entero.
        //
        // Pero si el contenido NO coincide, algo va mal de verdad: o la huella
        // se calculo sobre otra cosa o se guardo bajo la clave equivocada.  No
        // se espera que pase; se comprueba porque, si pasa, callarlo dejaria el
        // almacen sirviendo un nodo por otro y el fallo apareceria lejisimos de
        // aqui.
        if (it->second.payload != node.payload ||
            it->second.header.kind != node.header.kind) {
            return false;
        }
        return true;
    }
    nodes_.emplace(hash, node);
    return true;
}

bool MemoryNodeStore::get(ContentHash hash, StoredNode &out) const {
    auto it = nodes_.find(hash);
    if (it == nodes_.end()) return false;
    out = it->second;
    return true;
}

bool MemoryNodeStore::contains(ContentHash hash) const {
    return nodes_.find(hash) != nodes_.end();
}

// ---------------------------------------------------------------------------
//  En disco
// ---------------------------------------------------------------------------

std::string FileNodeStore::path_for(ContentHash hash) const {
    const std::string hex = hash.to_hex();
    // Los dos primeros digitos como subcarpeta: con decenas de miles de nodos,
    // un solo directorio se vuelve lento de recorrer en casi cualquier sistema
    // de ficheros.  Repartirlos lo evita sin mantener ningun indice.
    return root_ + "/" + hex.substr(0, 2) + "/" + hex;
}

bool FileNodeStore::put(const StoredNode &node) {
    namespace fs = std::filesystem;
    const ContentHash hash = node.header.hash;
    if (hash.empty()) return false; // un nodo sin huella no tiene donde ir
    const std::string path = path_for(hash);
    std::error_code ec;
    if (fs::exists(path, ec)) return true; // ya esta: nada que hacer

    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) return false;

    ByteWriter w;
    w.u32(VXDBG_NODE_MAGIC);
    w.u16(static_cast<uint16_t>(node.header.kind));
    w.u16(0); // relleno, para que el encabezado quede alineado a 4
    w.u32(node.header.schema_version);
    w.u32(static_cast<uint32_t>(node.payload.size()));
    w.raw(node.payload.data(), node.payload.size());

    // Se escribe a un temporal y se renombra: si el proceso muere a mitad, el
    // almacen no se queda con un fichero a medias que luego pasaria por bueno
    // -- y como el nombre es la huella, un fichero truncado seria un nodo que
    // miente sobre su propio contenido.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char *>(w.bytes().data()),
                static_cast<std::streamsize>(w.size()));
        if (!f) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        // Otro proceso pudo crearlo entre medias; con el mismo contenido, da
        // igual quien gane.
        fs::remove(tmp, ec);
        return fs::exists(path);
    }
    return true;
}

bool FileNodeStore::get(ContentHash hash, StoredNode &out) const {
    std::ifstream f(path_for(hash), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n <= 0) return false;
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    f.read(reinterpret_cast<char *>(bytes.data()), n);
    if (!f) return false;

    ByteReader r(bytes);
    if (r.u32() != VXDBG_NODE_MAGIC) return false;
    const uint16_t kind = r.u16();
    (void)r.u16(); // relleno
    const uint32_t schema = r.u32();
    const uint32_t len = r.u32();
    if (!r.ok()) return false;

    out.header.kind = static_cast<NodeKind>(kind);
    out.header.schema_version = schema;
    // La huella no se guarda dentro del fichero: ES su nombre.  Escribirla
    // ademas dentro seria repetir el dato y abrir la puerta a que discrepen.
    out.header.hash = hash;
    out.payload.resize(len);
    if (len > 0 && !r.raw(out.payload.data(), len)) return false;
    return r.ok();
}

bool FileNodeStore::contains(ContentHash hash) const {
    std::error_code ec;
    return std::filesystem::exists(path_for(hash), ec);
}

} // namespace vxdbg
