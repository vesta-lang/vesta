/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file memory_access.cpp
 * @brief Implementacion del vocabulario UNICO de acceso a memoria.  Un solo
 *        switch semantico LOAD/STORE/GETFIELD/ARRAY_x/MEMCPY que consumen todos
 *        los pases; memcpy NO es opaco (footprint dst+src conocido).
 */
#include "analysis/memory/memory_access.h"

#include "ir/ssa_ir.h"

namespace analysis {

using effects::AbstractLoc;

int32_t memory_access_size(ir::IrType t) {
    switch (t) {
    case ir::IrType::I8:
    case ir::IrType::U8:
    case ir::IrType::BOOL: return 1;
    case ir::IrType::I16:
    case ir::IrType::U16: return 2;
    case ir::IrType::I32:
    case ir::IrType::U32:
    case ir::IrType::F32:
    case ir::IrType::HANDLE: return 4;
    default: return 8; // I64/U64/F64/PTR/VOID: conservador
    }
}

namespace {
AbstractLoc unknown_loc() {
    return {AbstractLoc::Kind::Unknown, effects::LOC_GENERIC, 0, 0};
}
} // namespace

MemoryAccess memory_access(const ir::IrInstr &ins, const PointsTo &pt) {
    using Op = ir::IrOp;
    MemoryAccess a;
    const auto &ops = ins.operands;
    const int32_t w = memory_access_size(ins.type);

    switch (ins.op) {
    // --- Lecturas con direccion precisa (offset+ancho) ---
    case Op::LOAD:
        if (!ops.empty()) {
            a.touches = a.is_load = true;
            a.read_loc = loc_of(pt, ops[0], w);
        }
        return a;
    // --- Lecturas de cabecera de objeto (whole-object) ---
    case Op::GETFIELD:
    case Op::ARRAY_LOAD:
    case Op::ARRAY_LEN:
        if (!ops.empty()) {
            a.touches = a.is_load = true;
            a.read_loc = loc_of(pt, ops[0], 0);
        }
        return a;
    // --- Escrituras con direccion precisa ---
    case Op::STORE:
        if (ops.size() >= 2) {
            a.touches = a.is_store = true;
            a.write_loc = loc_of(pt, ops[1], w);
        }
        return a;
    // --- Escrituras a campo/elemento (whole-object) ---
    case Op::SETFIELD:
    case Op::ARRAY_STORE:
    case Op::GCWB_IR:
        if (!ops.empty()) {
            a.touches = a.is_store = true;
            a.write_loc = loc_of(pt, ops[0], 0);
        }
        return a;
    // --- MEMCPY: NO es opaco.  dst=ops[0], src=ops[1], len=ops[2] (const si
    //     cabe en 64 B, whole-root si no).  Lee src, escribe dst. ---
    case Op::MEMCPY:
        if (ops.size() >= 3) {
            // dst/src whole-root (width 0): el footprint se conoce (no opaco),
            // pero el offset exacto dentro del objeto no; disjuncion por RAiZ.
            a.touches = true;
            a.is_load = a.is_store = true;
            a.read_loc = loc_of(pt, ops[1], 0);  // src (whole-root)
            a.write_loc = loc_of(pt, ops[0], 0); // dst (whole-root)
        } else {
            a.touches = a.opaque = a.is_store = true;
            a.write_loc = unknown_loc();
        }
        return a;
    default:
        return a; // no es un acceso a memoria localizable
    }
}

} // namespace analysis
