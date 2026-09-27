/**
 * @file vxdbg_artifact.cpp
 * @brief El mapa del artefacto en el grafo de depuracion.
 * @see vx/project/vxdbg_artifact.h
 */
#include "vx/project/vxdbg_artifact.h"

#include "vx/compiler.h"
#include "vx/project/module_work.h"
#include "vx/vxdbg_emit.h" // default_vxdbg_dir
#include "vxdbg/codec.h"   // store_node
#include "vxdbg/pack_store.h"
#include "vxdbg/roots.h"

#include <memory>
#include <string>

namespace vx {

void compose_vxdbg_artifact(const std::vector<ProjectModuleWork> &work,
                            const CompileOptions &opts, CompileResult &res) {
    if (opts.ir_only || work.empty()) return;
    vxdbg::ArtifactMap map;
    for (const ProjectModuleWork &pm : work) {
        for (const vxdbg::SymbolLink &link : pm.vxdbg_symbols)
            map.add(link.symbol, link.entity);
        if (!pm.vxdbg_module_map.empty())
            map.modules.push_back(pm.vxdbg_module_map);
    }
    /* Empaquetado por delante, suelto detras: es como escribe la emision, y
     * este mapa CITA nodos que ella guardo.  Con el suelto solo, el
     * `contains` de aqui no veia lo que ya estaba en un paquete. */
    const std::string dir =
        opts.vxdbg_dir.empty() ? default_vxdbg_dir() : opts.vxdbg_dir;
    vxdbg::PackNodeStore store(
        dir, std::unique_ptr<vxdbg::NodeStore>(new vxdbg::FileNodeStore(dir)));
    vxdbg::ContentHash h;
    if (!map.symbols.empty() && vxdbg::store_node(store, map, h))
        res.vxdbg_artifact_map = h;
    /* Los tramos, del raiz: es el modulo del programa, y el unico que el
     * camino de fichero suelto tiene.  Los de los demas modulos no se
     * componen todavia (ver `doc/PLAN_GRAFO_SENTENCIAS.md`: la traza deberia
     * leer la coordenada del intermedio y no esta tabla). */
    res.vxdbg_span_map = work.back().vxdbg_span_map;
}

} // namespace vx
