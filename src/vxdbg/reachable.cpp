/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vxdbg/reachable.cpp
 * @brief Implementacion del recorrido (contrato en reachable.h).
 */

#include "vxdbg/reachable.h"

#include "util/file_read.h"
#include "vxdbg/codec.h"
#include "vxdbg/roots.h"
#include "vxdbg/source_meta.h"

#include <deque>
#include <filesystem>

namespace vxdbg {

namespace {

/// Anade @p id si apunta a algo.  Las referencias vacias son normales -- una
/// entidad sin cuerpo, una declarada sin fichero -- y no son un nodo.
template <typename Ref>
void push_if_set(const Ref &id, std::vector<ContentHash> &out) {
    if (!id.empty()) out.push_back(id.hash);
}

/// Las referencias de un mapa de artefacto: las entidades de sus simbolos.
ReachStatus references_of_artifact_map(const StoredNode &node,
                                       std::vector<ContentHash> &out) {
    ArtifactMap map;
    if (!decode(node, map)) return ReachStatus::Undecodable;
    for (const auto &sym : map.symbols)
        push_if_set(sym.second, out);
    return ReachStatus::Ok;
}

/// Las referencias de una entidad: con quien se relaciona, en que fichero se
/// declaro, y su cuerpo intermedio si lo tiene.
ReachStatus references_of_entity(const StoredNode &node,
                                 std::vector<ContentHash> &out) {
    LanguageEntity entity;
    if (!decode(node, entity)) return ReachStatus::Undecodable;
    for (const auto &rel : entity.relations)
        push_if_set(rel.target, out);
    push_if_set(entity.declared_at.file, out);
    push_if_set(entity.body, out);
    return ReachStatus::Ok;
}

} // namespace

ReachStatus collect_references(const StoredNode &node,
                               std::vector<ContentHash> &out) {
    switch (node.header.kind) {
    case NodeKind::ArtifactMap:
        return references_of_artifact_map(node, out);
    case NodeKind::Entity:
        return references_of_entity(node, out);

    /* Hojas.  Se nombran una a una y NO se agrupan en el `default`: asi, el dia
     * que aparezca un genero nuevo, cae en el `default` y aborta el recorrido
     * en vez de pasar por hoja.  Un genero nuevo tratado como hoja es
     * exactamente el fallo que este modulo existe para evitar. */
    case NodeKind::File:
        // El `checksum` de un fichero es la huella de SU TEXTO, no la de un
        // nodo del grafo: seguirla buscaria en el almacen algo que nunca se
        // guardo ahi.
        return ReachStatus::Ok;
    case NodeKind::SpanMap:
        // Tramos por simbolo y linea: cadenas y numeros, ningun nodo.
        return ReachStatus::Ok;

    default:
        return ReachStatus::UnknownKind;
    }
}

size_t read_published_roots(const std::string &cache_dir,
                            std::vector<ContentHash> &out_roots) {
    namespace stdfs = std::filesystem;
    const std::string dir = cache_dir + "/roots";
    std::error_code ec;
    if (!stdfs::exists(dir, ec)) return 0;

    // Son miles de ficheros diminutos, que es justo el caso para el que existe
    // DirectoryReader: el directorio se abre una vez y cada apuntador se lee
    // dando solo su nombre.
    const util::DirectoryReader reader(dir);
    size_t read = 0;
    for (const auto &entry : stdfs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string path = entry.path().string();
        if (path.size() < 4 || path.compare(path.size() - 4, 4, ".ptr") != 0)
            continue;

        std::vector<uint8_t> bytes;
        const std::string leaf = entry.path().filename().string();
        if (!(reader.ok() ? reader.read_file(leaf, bytes)
                          : util::read_whole_file(path, bytes)))
            continue;

        /* Tres lineas: mapa, de quien es, y tramos.  La segunda no es una
         * referencia sino la identidad del artefacto -- sirve para notar que un
         * apuntador acabo donde no tocaba --, asi que no entra como raiz. */
        const std::string body(reinterpret_cast<const char *>(bytes.data()),
                               bytes.size());
        const size_t nl1 = body.find('\n');
        if (nl1 == std::string::npos) continue;
        const ContentHash map = ContentHash::from_hex(body.substr(0, nl1));
        if (map.empty()) continue;
        ++read;
        out_roots.push_back(map);

        const size_t nl2 = body.find('\n', nl1 + 1);
        if (nl2 == std::string::npos) continue;
        const size_t nl3 = body.find('\n', nl2 + 1);
        const std::string spans_hex =
            nl3 == std::string::npos ? body.substr(nl2 + 1)
                                     : body.substr(nl2 + 1, nl3 - nl2 - 1);
        const ContentHash spans = ContentHash::from_hex(spans_hex);
        // Los tramos son opcionales: una compilacion sin ellos publica la
        // huella vacia.
        if (!spans.empty()) out_roots.push_back(spans);
    }
    return read;
}

ReachReport compute_live_set(const NodeStore &store,
                             const std::vector<ContentHash> &roots,
                             std::set<ContentHash> &out_live) {
    ReachReport report;
    report.roots_read = roots.size();
    std::deque<ContentHash> pending;
    for (const auto &r : roots) {
        if (r.empty()) continue;
        if (out_live.insert(r).second) pending.push_back(r);
    }

    std::vector<ContentHash> refs;
    while (!pending.empty()) {
        const ContentHash current = pending.front();
        pending.pop_front();

        StoredNode node;
        if (!store.get(current, node)) {
            /* Colgada: se cita algo que no esta.  No es un fallo -- si no esta,
             * no hay nada suyo que conservar -- y pasa de verdad, porque una
             * entidad puede citar su funcion intermedia y esos nodos hoy no se
             * emiten.  Se cuenta para que se vea. */
            ++report.dangling_refs;
            out_live.erase(current);
            continue;
        }
        ++report.nodes_reached;

        refs.clear();
        const ReachStatus st = collect_references(node, refs);
        if (st != ReachStatus::Ok) {
            /* Se para AQUI y se dice cual.  Seguir daria un conjunto vivo
             * incompleto, y con el la reclamacion borraria datos que si se
             * usaban.  Vale mas un cache que crece que uno que miente. */
            report.status = st;
            report.blocking_kind = node.header.kind;
            report.blocking_node = current;
            return report;
        }
        for (const auto &r : refs) {
            if (r.empty()) continue;
            if (out_live.insert(r).second) pending.push_back(r);
        }
    }
    return report;
}

} // namespace vxdbg
