/**
 * @file module_artifact.cpp
 * @brief El artefacto en cache de un modulo y su adopcion.
 * @see vx/project/module_artifact.h
 */
#include "vx/project/module_artifact.h"

#include "ir/ssa_ir_serialize.h" // ir::adopt_cached_module
#include "vx/project/module_work.h"

#include <utility>

namespace vx {

namespace {

/// Anade @p v a @p out en little-endian.
void put_u32_le(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

/**
 * @brief Lee un u32 little-endian de @p blob en @p off y avanza.
 * @return false si no quedan cuatro bytes.
 */
bool get_u32_le(const std::vector<uint8_t> &blob, size_t &off, uint32_t &v) {
    if (off + 4 > blob.size()) return false;
    v = static_cast<uint32_t>(blob[off]) |
        (static_cast<uint32_t>(blob[off + 1]) << 8) |
        (static_cast<uint32_t>(blob[off + 2]) << 16) |
        (static_cast<uint32_t>(blob[off + 3]) << 24);
    off += 4;
    return true;
}

} // namespace

std::vector<uint8_t> pack_module_artifact(const std::vector<uint8_t> &vxi,
                                          const std::vector<uint8_t> &vxir) {
    std::vector<uint8_t> out;
    out.reserve(8 + vxi.size() + vxir.size());
    put_u32_le(out, static_cast<uint32_t>(vxi.size()));
    out.insert(out.end(), vxi.begin(), vxi.end());
    put_u32_le(out, static_cast<uint32_t>(vxir.size()));
    out.insert(out.end(), vxir.begin(), vxir.end());
    return out;
}

bool unpack_module_artifact(const std::vector<uint8_t> &blob,
                            std::vector<uint8_t> &vxi,
                            std::vector<uint8_t> &vxir) {
    size_t off = 0;
    uint32_t vl = 0, il = 0;
    if (!get_u32_le(blob, off, vl) || off + vl > blob.size()) return false;
    vxi.assign(blob.begin() + off, blob.begin() + off + vl);
    off += vl;
    if (!get_u32_le(blob, off, il) || off + il > blob.size()) return false;
    vxir.assign(blob.begin() + off, blob.begin() + off + il);
    return true;
}

void adopt_cached_artifact(ProjectModuleWork &pm, VxiModule &&vxi,
                           ir::IrModule &&ir) {
    /* Lo que sale de una cache se comprueba igual que lo recien construido.
     * Es el sitio donde un IR mal guardado deja de ser un problema de quien lo
     * guardo y pasa a ser el de quien lo usa: aqui ya no hay fuente al que
     * volver, y lo que venga se optimiza y se emite tal cual.  (Solo lo hacia
     * el acierto del almacen comun; el de la cache por ruta, no.) */
    ir::ir_verify_if_asked(ir, "cache", pm.module_name.str());
    pm.vxi = std::move(vxi);
    /* v20: si declaraba clases, que el tree-shake lo preguntara despues y aqui
     * no hay AST al que preguntarselo. */
    pm.has_classes = pm.vxi.declares_classes;
    /* v18: el conjunto comptime, del `.vxi`.  Un modulo servido de la cache NO
     * se parsea, asi que no hay AST del que extraerlo: sin esto el conjunto del
     * proyecto salia con un modulo de siete -- el unico recompilado -- y nada
     * lo decia.
     *
     * "Aporta algo" es el TEXTO o los NOMBRES, con el mismo criterio con que
     * se junta despues el conjunto del proyecto: un modulo cuyo conjunto son
     * solo constantes comptime trae nombres y ningun texto.  Mirando solo el
     * texto, servido de la cache perdia sus nombres -- y con ellos el criterio
     * de pertenencia al emitir el artefacto comptime --, mientras que
     * compilado de nuevo los conservaba. */
    if (!pm.vxi.comptime_unit_source.empty() ||
        !pm.vxi.comptime_unit_names.empty()) {
        pm.comptime_unit_source += pm.vxi.comptime_unit_source;
        pm.comptime_unit_names.insert(pm.comptime_unit_names.end(),
                                      pm.vxi.comptime_unit_names.begin(),
                                      pm.vxi.comptime_unit_names.end());
        pm.comptime_unit_hash = pm.vxi.comptime_unit_hash;
    }
    pm.comptime_unit_not_collected.insert(
        pm.comptime_unit_not_collected.end(),
        pm.vxi.comptime_unit_not_collected.begin(),
        pm.vxi.comptime_unit_not_collected.end());
    /* TODO lo que el modulo restaurado trae, en una sola linea y decidido donde
     * se conoce el formato.  Campo a campo se olvido TRES veces --
     * `static_data`+`globals`, `native_imports` y `source_files` --, y ninguna
     * dio error. */
    ir::adopt_cached_module(pm.ir, std::move(ir));
    pm.ok = true;
}

} // namespace vx
