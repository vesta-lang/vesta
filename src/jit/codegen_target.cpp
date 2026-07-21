/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file jit/codegen_target.cpp
 * @brief Implementacion comun de @ref CodegenTarget (peephole generico).
 */

#include "jit/codegen_target.h"

namespace jit {

void CodegenTarget::peephole(MFunction &pf) const {
    // Borra los self-moves (MOV rX, rX) que el coalescing dejo al asignar el
    // mismo fisico a los dos extremos de una copia.  Generico multi-ISA.
    for (MBlock &blk : pf.blocks) {
        std::vector<MInstr> keep;
        keep.reserve(blk.instrs.size());
        for (MInstr &mi : blk.instrs) {
            const bool self_move =
                (mi.op == MOp::MOV || mi.op == MOp::A64_FMOV ||
                 mi.op == MOp::MOVSD || mi.op == MOp::MOVSS) &&
                mi.dst.kind == MOperandKind::REG &&
                mi.src1.kind == MOperandKind::REG && mi.dst.reg == mi.src1.reg &&
                mi.src2.kind == MOperandKind::NONE;
            if (!self_move) keep.push_back(mi);
        }
        blk.instrs = std::move(keep);
    }
}

} // namespace jit
