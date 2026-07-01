/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/peephole.cpp
 * @brief Implementacion del peephole sobre MachineIR fisico (ver peephole.h).
 */

#include "jit/peephole.h"

#include <cstdlib>
#include <vector>

namespace jit {

namespace {

bool peephole_disabled() noexcept {
    static const bool off = [] {
        const char *v = std::getenv("VESTA_NO_PEEPHOLE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return off;
}

/// True si @p in es un SELF-MOVE redundante y eliminable: copia reg->reg
/// del MISMO registro fisico, en un ancho que no tiene efecto observable.
///   - MOV de 64-bit (`mov rX, rX`): nop puro.
///   - MOVSD/MOVSS (`movsd xX, xX`): nop (no toca bits altos relevantes).
/// Un `mov eX, eX` (32-bit) zero-extiende -> NO eliminable; se conserva.
bool is_redundant_self_move(const MInstr &in) noexcept {
    if (in.src2.kind != MOperandKind::NONE) return false;
    if (in.dst.kind != MOperandKind::REG || in.src1.kind != MOperandKind::REG)
        return false;
    if (in.dst.reg != in.src1.reg) return false;
    if (in.op == MOp::MOV) return in.dst.width == 8;
    if (in.op == MOp::MOVSD || in.op == MOp::MOVSS) return true;
    return false;
}

} // namespace

uint32_t peephole_physical(MFunction &pf) {
    if (peephole_disabled()) return 0;
    uint32_t removed = 0;
    for (MBlock &b : pf.blocks) {
        std::vector<MInstr> kept;
        kept.reserve(b.instrs.size());
        for (const MInstr &in : b.instrs) {
            if (is_redundant_self_move(in)) {
                ++removed;
                continue;
            }
            kept.push_back(in);
        }
        if (kept.size() != b.instrs.size()) b.instrs = std::move(kept);
    }
    return removed;
}

} // namespace jit
