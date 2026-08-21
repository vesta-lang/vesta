/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vxdbg/store_cli.cpp
 * @brief Implementacion del subcomando (contrato en store_cli.h).
 */

#include "vxdbg/store_cli.h"

#include "vx/vxdbg_emit.h"
#include "vxdbg/maintenance.h"
#include "vxdbg/pack_store.h"
#include "vxdbg/reachable.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <vector>
#include <string>

namespace vxdbg {
namespace cli {

namespace {

/// Lo que ocupa el almacen en disco, contado del propio disco.
struct StoreSize {
    size_t packs = 0;
    uint64_t pack_bytes = 0;
    size_t loose = 0;
    size_t roots = 0;
};

StoreSize measure_store(const std::string &dir) {
    namespace stdfs = std::filesystem;
    StoreSize size;
    std::error_code ec;

    for (const auto &e : stdfs::directory_iterator(dir + "/packs", ec)) {
        if (ec) break;
        const std::string p = e.path().string();
        if (p.size() < 5 || p.compare(p.size() - 5, 5, ".vxpk") != 0) continue;
        ++size.packs;
        const auto n = e.file_size(ec);
        if (!ec) size.pack_bytes += static_cast<uint64_t>(n);
        ec.clear();
    }
    ec.clear();

    for (const auto &e : stdfs::directory_iterator(dir + "/roots", ec)) {
        if (ec) break;
        if (e.is_regular_file(ec)) ++size.roots;
    }
    ec.clear();

    /* Los sueltos van repartidos en carpetas de dos digitos, como los objetos
     * de git, asi que hay que bajar un nivel.  Se salta `packs` y `roots`, que
     * viven al mismo nivel y no son eso. */
    for (const auto &e : stdfs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
        const std::string name = e.path().filename().string();
        if (name == "packs" || name == "roots") continue;
        std::error_code ec2;
        for (const auto &f : stdfs::directory_iterator(e.path(), ec2)) {
            if (ec2) break;
            if (f.is_regular_file(ec2)) ++size.loose;
        }
    }
    return size;
}

const char *reason_text(ReachStatus st) {
    switch (st) {
    case ReachStatus::UnknownKind:
        return "un genero de nodo que el recorrido no sabe seguir";
    case ReachStatus::Undecodable:
        return "un nodo que el codec rechazo (esquema de otra version?)";
    case ReachStatus::Ok:
        break;
    }
    return "";
}

void print_usage() {
    std::printf(
        "uso: vm vxdbg <orden> [argumento] [carpeta]\n\n"
        "  status          Que hay en el almacen y cuanto sobra.  No borra.\n"
        "  gc              Borra lo que ya no se usa y junta el resto.  SI "
        "borra.\n"
        "  node <huella>   Que es ese nodo y a quien cita.\n"
        "  dangling        Referencias a nodos que no estan, CON quien las "
        "hace.\n\n"
        "La carpeta por defecto es la del cache del proyecto.\n");
}

/// Nombre legible de un genero de nodo.  Solo los que se escriben hoy; del
/// resto se da el numero, que es mejor que inventarle un nombre.
const char *kind_name(NodeKind k) {
    switch (k) {
    case NodeKind::Entity: return "Entity";
    case NodeKind::File: return "File";
    case NodeKind::ArtifactMap: return "ArtifactMap";
    case NodeKind::SpanMap: return "SpanMap";
    default: return nullptr;
    }
}

void print_kind(NodeKind k) {
    const char *n = kind_name(k);
    if (n != nullptr) std::printf("%s", n);
    else std::printf("genero %u", static_cast<unsigned>(k));
}

int run_node(const std::string &dir, const std::string &hex) {
    const ContentHash h = ContentHash::from_hex(hex);
    if (h.empty()) {
        std::printf("huella invalida: %s\n", hex.c_str());
        return 2;
    }
    PackNodeStore store(dir,
                        std::unique_ptr<NodeStore>(new FileNodeStore(dir)));
    StoredNode node;
    if (!store.get(h, node)) {
        std::printf("%s: NO esta en el almacen\n", hex.c_str());
        return 1;
    }
    std::printf("%s\n  genero    ", hex.c_str());
    print_kind(node.header.kind);
    std::printf("\n  esquema   v%u\n  bytes     %zu\n",
                node.header.schema_version, node.payload.size());

    /* Y a quien cita, con la MISMA rutina que usa el recorrido.  Si se listaran
     * aparte, un dia una diria una cosa y la otra otra, y esta orden existe
     * justo para poder creerse lo que dice el recorrido. */
    std::vector<ContentHash> refs;
    const ReachStatus st = collect_references(node, refs);
    if (st != ReachStatus::Ok) {
        std::printf("  cita      no se sabe: %s\n", reason_text(st));
        return 1;
    }
    std::printf("  cita      %zu\n", refs.size());
    for (const auto &r : refs) {
        StoredNode dest;
        const bool esta = store.get(r, dest);
        std::printf("    %s  %s", r.to_hex().c_str(), esta ? "" : "[NO ESTA] ");
        if (esta) print_kind(dest.header.kind);
        std::printf("\n");
    }
    return 0;
}

int run_dangling(const std::string &dir) {
    namespace stdfs = std::filesystem;
    std::error_code ec;
    if (!stdfs::exists(dir, ec)) {
        std::printf("no hay almacen en %s\n", dir.c_str());
        return 0;
    }
    PackNodeStore store(dir,
                        std::unique_ptr<NodeStore>(new FileNodeStore(dir)));
    std::vector<ContentHash> roots;
    read_published_roots(dir, roots);
    std::set<ContentHash> live;
    // Aqui SI se pide la procedencia: esta orden existe para eso.  El
    // mantenimiento que corre al compilar la pide a cero y no paga nada.
    const ReachReport report =
        compute_live_set(store, roots, live, kMaxDanglingSamples);

    std::printf("referencias sin destino: %zu\n", report.dangling_refs);
    if (report.dangling_refs == 0) return 0;
    std::printf("muestra de %zu, con quien las hace:\n\n",
                report.dangling_samples.size());
    for (const auto &d : report.dangling_samples) {
        std::printf("  falta %s\n", d.missing.to_hex().c_str());
        if (d.from.empty()) {
            // Sin procedencia: era una raiz, y a esas las cita el apuntador,
            // que esta fuera del grafo a proposito.
            std::printf("    la cita una RAIZ (un .ptr), no un nodo\n");
        } else {
            std::printf("    la cita %s (", d.from.to_hex().c_str());
            print_kind(d.from_kind);
            std::printf(")\n");
        }
    }
    std::printf("\nPara ver quien cita:  vm vxdbg node <huella>\n");
    return 0;
}

int run_gc(const std::string &dir) {
    /* La MISMA rutina que corre sola al compilar, forzada.  Dos codigos que
     * recogen el almacen acabarian recogiendo cosas distintas, y el que se
     * usa a diario -- el automatico -- seria justo el que nadie prueba a
     * mano. */
    const MaintenanceResult r = maintain_store(dir, 0, /*force=*/true);
    switch (r.status) {
    case MaintenanceStatus::NoStore:
        std::printf("no hay almacen en %s\n", dir.c_str());
        return 0;
    case MaintenanceStatus::NoRoots:
        std::printf("sin raices publicadas: no se toca nada.\n"
                    "  Que no haya raices no quiere decir que no haya nada "
                    "vivo; quiere decir que no se sabe.\n");
        return 1;
    case MaintenanceStatus::TraversalIncomplete:
        std::printf("no se pudo saber que vive; no se toca nada.\n"
                    "  `vm vxdbg status` dice que lo bloquea.\n");
        return 1;
    case MaintenanceStatus::WriteFailed:
        std::printf("fallo al escribir los paquetes nuevos; los viejos se "
                    "quedan como estaban.\n");
        return 1;
    case MaintenanceStatus::BelowThreshold:
    case MaintenanceStatus::Ran:
        break;
    }
    std::printf("raices retiradas (sobrescritas): %zu\n", r.roots_retired);
    std::printf("paquetes borrados enteros: %zu\n", r.packs_removed);
    std::printf("compactado: %zu paquetes -> %zu   (%.1f MiB -> %.1f MiB)\n",
                r.packs_before, r.packs_after, r.bytes_before / 1048576.0,
                r.bytes_after / 1048576.0);
    std::printf("  entradas conservadas %zu, descartadas %zu\n", r.entries_kept,
                r.entries_dropped);
    return 0;
}

int run_status(const std::string &dir) {
    namespace stdfs = std::filesystem;
    std::error_code ec;
    if (!stdfs::exists(dir, ec)) {
        std::printf("no hay almacen en %s\n", dir.c_str());
        return 0;
    }

    const StoreSize size = measure_store(dir);
    std::printf("almacen: %s\n", dir.c_str());
    std::printf("  paquetes         %8zu   (%.1f MiB)\n", size.packs,
                size.pack_bytes / 1048576.0);
    std::printf("  objetos sueltos  %8zu\n", size.loose);
    std::printf("  raices           %8zu\n", size.roots);

    if (size.roots == 0) {
        /* Sin raices TODO estaria muerto, y reclamar vaciaria el almacen.  Se
         * dice en vez de dar un cero que parece un resultado. */
        std::printf("\nsin raices publicadas: no se puede decir que sobra.\n");
        return 0;
    }

    PackNodeStore store(
        dir, std::unique_ptr<NodeStore>(new FileNodeStore(dir)));

    std::vector<ContentHash> roots;
    read_published_roots(dir, roots);

    std::set<ContentHash> live;
    const ReachReport report = compute_live_set(store, roots, live);

    std::printf("\nrecorrido desde las raices:\n");
    std::printf("  entradas          %8zu\n", report.roots_read);
    std::printf("  nodos alcanzados  %8zu\n", report.nodes_reached);
    std::printf("  referencias sin destino %4zu\n", report.dangling_refs);

    if (report.status != ReachStatus::Ok) {
        /* Se para y se dice por que.  Seguir daria un conjunto vivo incompleto
         * y con el se borraria lo que si se usa. */
        std::printf("\nel recorrido NO se completo: %s\n",
                    reason_text(report.status));
        std::printf("  genero  %u\n  nodo    %s\n",
                    static_cast<unsigned>(report.blocking_kind),
                    report.blocking_node.to_hex().c_str());
        std::printf("\nmientras siga asi no se puede reclamar nada sin "
                    "arriesgarse a borrar lo vivo.\n");
        return 1;
    }

    const PackNodeStore::ReclaimPreview preview = store.preview_reclaim(live);
    std::printf("\nsi se reclamara AHORA (con estas raices):\n");
    std::printf("  entradas indexadas %7zu, vivas %zu\n", preview.entries,
                preview.live_entries);
    std::printf("  paquetes a borrar  %7zu de %zu   (%.1f MiB)\n",
                preview.packs_to_delete, preview.packs,
                preview.bytes_to_free / 1048576.0);
    if (preview.packs_to_delete == 0)
        std::printf("\nninguno: cada paquete conserva alguna entrada viva.\n"
                    "  Mientras no se retire ninguna raiz, esto seguira asi:\n"
                    "  cada compilacion publica la suya y nada la quita.\n");
    return 0;
}

} // namespace

int run(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }
    const std::string order = argv[1];

    // `node` lleva la huella como primer argumento, asi que su carpeta -- si se
    // da -- es el segundo.  El resto la llevan en el primero.
    if (order == "node") {
        if (argc < 3) {
            print_usage();
            return 2;
        }
        const std::string dir = argc >= 4 ? argv[3] : vx::default_vxdbg_dir();
        return run_node(dir, argv[2]);
    }

    const std::string dir = argc >= 3 ? argv[2] : vx::default_vxdbg_dir();
    if (order == "status") return run_status(dir);
    if (order == "gc") return run_gc(dir);
    if (order == "dangling") return run_dangling(dir);

    std::printf("orden desconocida: %s\n\n", order.c_str());
    print_usage();
    return 2;
}

} // namespace cli
} // namespace vxdbg
