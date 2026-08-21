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
#include "vxdbg/pack_store.h"
#include "vxdbg/reachable.h"

#include <cstdio>
#include <filesystem>
#include <memory>
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
    std::printf("uso: vm vxdbg <orden> [carpeta]\n\n"
                "  status   Que hay en el almacen y cuanto sobra.  No borra.\n\n"
                "La carpeta por defecto es la del cache del proyecto.\n");
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
    const std::string dir = argc >= 3 ? argv[2] : vx::default_vxdbg_dir();

    if (order == "status") return run_status(dir);

    std::printf("orden desconocida: %s\n\n", order.c_str());
    print_usage();
    return 2;
}

} // namespace cli
} // namespace vxdbg
