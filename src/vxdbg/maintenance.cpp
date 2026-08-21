/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vxdbg/maintenance.cpp
 * @brief Implementacion del mantenimiento (contrato en maintenance.h).
 */

#include "vxdbg/maintenance.h"

#include "vxdbg/pack_store.h"
#include "vxdbg/reachable.h"
#include "vxdbg/store.h"


#include <filesystem>

#include <memory>
#include <set>
#include <vector>

namespace vxdbg {

namespace {

/// Cuenta de paquetes: cuantos hay, y cuales se pueden tocar.
struct PackCount {
    size_t total = 0;
    /// Los de compilaciones YA TERMINADAS.  De los de las que siguen en marcha
    /// no se sabe todavia si su raiz llegara, asi que no se tocan.
    std::set<std::string> collectable;
};

/// Cuenta los paquetes sin abrir ninguno.
PackCount count_packs(const std::string &dir) {
    namespace stdfs = std::filesystem;
    std::error_code ec;
    PackCount count;
    for (const auto &e : stdfs::directory_iterator(dir + "/packs", ec)) {
        if (ec) break;
        const std::string p = e.path().string();
        if (p.size() < 5 || p.compare(p.size() - 5, 5, ".vxpk") != 0) continue;
        ++count.total;
        if (!pack_writer_is_running(p)) count.collectable.insert(p);
    }
    return count;
}

} // namespace

MaintenanceResult maintain_store(const std::string &dir, size_t pack_threshold,
                                 bool force) {
    namespace stdfs = std::filesystem;
    MaintenanceResult result;
    std::error_code ec;
    if (!stdfs::exists(dir, ec)) {
        result.status = MaintenanceStatus::NoStore;
        return result;
    }

    /* Lo primero es lo barato: contar ficheros.  Este camino lo recorre CADA
     * compilacion, asi que mientras no haga falta recoger no puede costar mas
     * que enumerar un directorio. */
    /* ESTE ORDEN ES LA PARTE DELICADA DE TODO EL FICHERO.
     *
     * Primero se fija QUE paquetes se pueden tocar -- los de procesos que ya
     * han terminado -- y solo DESPUES se leen las raices.  Al reves se pierden
     * datos, y esta visto pasar con la suite en paralelo: un proceso escribe su
     * paquete, sigue vivo mientras se le comprueba, publica su raiz y muere.
     * Si las raices se leyeron antes de que la publicara, su contenido no sale
     * en el conjunto vivo; y si la comprobacion se hace al borrar, para
     * entonces ya esta muerto y su paquete ha dejado de estar protegido.  Se
     * borraria justo lo que acaba de anunciar.
     *
     * Fijandolo antes, la regla se sostiene sola: si un proceso ya estaba
     * muerto cuando se miro, su raiz esta publicada y la lectura de despues la
     * va a ver.  Y el que muera despues no esta en el conjunto, asi que no se
     * le toca.
     *
     * Se decide ademas por los TOCABLES y no por el total: en una tanda
     * paralela casi todos son de procesos vivos, y mirar el total llevaria a
     * recorrer el grafo entero en cada compilacion para acabar sin poder borrar
     * nada. */
    const PackCount count = count_packs(dir);
    result.packs_before = count.total;
    if (!force && count.collectable.size() < pack_threshold) {
        result.status = MaintenanceStatus::BelowThreshold;
        return result;
    }

    PackNodeStore store(dir,
                        std::unique_ptr<NodeStore>(new FileNodeStore(dir)));

    std::vector<ContentHash> roots;
    read_published_roots(dir, roots);
    if (roots.empty()) {
        /* Sin raices el recorrido no alcanza nada y TODO pareceria muerto:
         * recoger aqui vaciaria el almacen entero.  Que no haya raices no
         * significa que no haya nada vivo, significa que no se sabe. */
        result.status = MaintenanceStatus::NoRoots;
        return result;
    }

    std::set<ContentHash> live;
    const ReachReport report = compute_live_set(store, roots, live);
    if (report.status != ReachStatus::Ok) {
        // Con un conjunto vivo incompleto se borraria lo que si se usa.
        result.status = MaintenanceStatus::TraversalIncomplete;
        return result;
    }

    /* Reclamar antes de compactar: reclamar borra paquetes enteros sin leerlos,
     * asi que quitarlos primero es trabajo que la compactacion ya no hace. */
    result.packs_removed = store.reclamar(live, &count.collectable);

    const PackNodeStore::CompactResult compacted =
        store.compact(live, 8192, &count.collectable);
    if (!compacted.ok) {
        result.status = MaintenanceStatus::WriteFailed;
        return result;
    }
    result.status = MaintenanceStatus::Ran;
    result.packs_after = compacted.packs_after;
    result.entries_kept = compacted.entries_kept;
    result.entries_dropped = compacted.entries_dropped;
    result.bytes_before = compacted.bytes_before;
    result.bytes_after = compacted.bytes_after;
    return result;
}

} // namespace vxdbg
