/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file lowering_map.cpp
 * @brief Consulta de la bajada, en los dos sentidos.
 */

#include "vxdbg/lowering_map.h"

namespace vxdbg {

void LoweringMap::build_index() {
    by_instr_.clear();
    for (const auto &e : entries) {
        for (const auto &i : e.ir_instrs) {
            by_instr_[i].push_back(e.statement);
        }
    }
}

const LoweringEntry *LoweringMap::of_statement(StatementId stmt) const {
    for (const auto &e : entries) {
        if (e.statement == stmt) return &e;
    }
    return nullptr;
}

std::vector<StatementId> LoweringMap::statements_of(IrInstrId instr) const {
    auto it = by_instr_.find(instr);
    if (it == by_instr_.end()) return {};
    return it->second;
}

} // namespace vxdbg
