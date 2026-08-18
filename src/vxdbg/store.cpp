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

ContentHash seal(StoredNode &node) {
    // La huella sale SOLO del contenido, no del encabezado: el genero y la
    // version describen como leerlo, no que es.  Dos nodos con el mismo
    // contenido son el mismo nodo.
    node.header.hash = hash_bytes(node.payload.data(), node.payload.size());
    return node.header.hash;
}

namespace {

/**
 * @brief Escribe el encabezado del fichero.
 * @param w Donde escribir.
 * @param h Encabezado.
 */
void write_file_header(ByteWriter &w, const NodeFileHeader &h) {
    w.u32(h.magic);
    w.u16(h.kind);
    w.u16(h.reserved);
    w.u32(h.schema_version);
    w.u32(h.payload_size);
}

/**
 * @brief Lee el encabezado del fichero.
 * @param r De donde leer.
 * @param out Recibe el encabezado.
 * @return @c true si se leyo entero y el magic cuadra.
 */
bool read_file_header(ByteReader &r, NodeFileHeader &out) {
    out.magic = r.u32();
    out.kind = r.u16();
    out.reserved = r.u16();
    out.schema_version = r.u32();
    out.payload_size = r.u32();
    return r.ok() && out.magic == VXDBG_NODE_MAGIC;
}

} // namespace

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

    NodeFileHeader fh;
    fh.magic = VXDBG_NODE_MAGIC;
    fh.kind = static_cast<uint16_t>(node.header.kind);
    fh.schema_version = node.header.schema_version;
    fh.payload_size = static_cast<uint32_t>(node.payload.size());

    ByteWriter w;
    w.reserve(VXDBG_NODE_HEADER_SIZE + node.payload.size());
    write_file_header(w, fh);
    w.raw(node.payload.data(), node.payload.size());

    // Se escribe a un temporal y se renombra: si el proceso muere a mitad, el
    // almacen no se queda con un fichero a medias que luego pasaria por bueno
    // -- y como el nombre es la huella, un fichero truncado seria un nodo que
    // miente sobre su propio contenido.
    //
    // El renombrado es atomico en POSIX; en Windows tiene matices cuando el
    // destino ya existe.  Aqui no llega a darse porque justo antes se comprueba
    // que no esta, y si otro proceso se adelanta se trata mas abajo: con el
    // mismo contenido, da igual quien gane.
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
    NodeFileHeader fh;
    if (!read_file_header(r, fh)) return false;

    out.header.kind = static_cast<NodeKind>(fh.kind);
    out.header.schema_version = fh.schema_version;
    // La huella no se guarda dentro del fichero: ES su nombre.  Escribirla
    // ademas dentro seria repetir el dato y abrir la puerta a que discrepen.
    out.header.hash = hash;
    out.payload.resize(fh.payload_size);
    if (fh.payload_size > 0 && !r.raw(out.payload.data(), fh.payload_size)) {
        return false;
    }
    if (!r.ok()) return false;

    if (verify_) {
        // El contenido tiene que corresponder al nombre.  Un fichero alterado
        // -- por un disco que falla o por alguien que lo toco -- se serviria
        // como si fuera el nodo que se pedia, y a partir de ahi todo lo que se
        // explique con el seria falso.
        const auto real = hash_bytes(out.payload.data(), out.payload.size());
        if (!(real == hash)) return false;
    }
    return true;
}

bool FileNodeStore::contains(ContentHash hash) const {
    std::error_code ec;
    return std::filesystem::exists(path_for(hash), ec);
}

} // namespace vxdbg
