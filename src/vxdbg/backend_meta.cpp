/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file backend_meta.cpp
 * @brief Consulta del codigo generado y de donde quedo colocado.
 */

#include "vxdbg/backend_meta.h"
#include "vxdbg/source_meta.h"

#include <algorithm>

namespace vxdbg {

std::vector<IrInstrId> CodeDebug::ir_at(uint32_t offset) const {
    // Los tramos van ordenados por inicio, asi que la busqueda binaria da el
    // ultimo que empieza en el desplazamiento o antes.
    auto it = std::upper_bound(
        ranges.begin(), ranges.end(), offset,
        [](uint32_t o, const CodeRange &r) { return o < r.begin; });
    if (it == ranges.begin()) return {};
    --it;
    if (offset >= it->end) return {}; // cae en un hueco entre tramos
    return it->ir_instrs;
}

LocationRange VariableMap::at(uint32_t position) const {
    // Un tramo de verdad: [from, to).  Antes se comparaban los extremos por
    // igualdad -- lo unico que se puede hacer con huellas -- y la variable
    // parecia existir solo justo al principio y justo al final, nunca en medio,
    // que es donde se pregunta.
    for (const auto &l : locations) {
        if (position >= l.from && position < l.to) return l;
    }
    // No consta donde vive ahi.  Se dice explicitamente en vez de devolver un
    // sitio inventado: "no se sabe" y "esta en el registro 0" son respuestas
    // muy distintas para quien mira un fallo.
    LocationRange none;
    none.kind = LocationKind::OptimizedOut;
    return none;
}

void PlacementMap::add(CodePlacement p) {
    placements_.push_back(p);
    sorted_ = false;
}

CodeId PlacementMap::find(uint64_t address, uint32_t &out_offset) const {
    if (!sorted_) {
        std::sort(placements_.begin(), placements_.end(),
                  [](const CodePlacement &a, const CodePlacement &b) {
                      return a.base < b.base;
                  });
        sorted_ = true;
    }
    auto it = std::upper_bound(
        placements_.begin(), placements_.end(), address,
        [](uint64_t a, const CodePlacement &p) { return a < p.base; });
    if (it == placements_.begin()) return CodeId{};
    --it;
    if (address >= it->base + it->size) return CodeId{};
    out_offset = static_cast<uint32_t>(address - it->base);
    return it->code;
}

uint32_t SessionMap::new_generation() {
    generations_.emplace_back();
    return static_cast<uint32_t>(generations_.size() - 1);
}

PlacementMap &SessionMap::generation(uint32_t gen) {
    while (generations_.size() <= gen)
        generations_.emplace_back();
    return generations_[gen];
}

CodeId SessionMap::find(uint64_t address, uint32_t &out_offset,
                        uint32_t &out_generation) const {
    // De la mas reciente a la mas antigua: si una direccion esta en varias
    // revisiones, interesa la ultima.  Pero se miran TODAS, porque un marco
    // puede seguir dentro de una version del codigo que ya se sustituyo -- y
    // es justo la que hay que explicar.
    for (size_t i = generations_.size(); i > 0; --i) {
        const CodeId c = generations_[i - 1].find(address, out_offset);
        if (!c.empty()) {
            out_generation = static_cast<uint32_t>(i - 1);
            return c;
        }
    }
    return CodeId{};
}

} // namespace vxdbg
