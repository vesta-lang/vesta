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

int32_t memory_access_size_bytes(int32_t raw) {
    // Escalar (1/2/4/8) o vectorial (16/32/64: XMM/YMM/ZMM).  Otro -> 0 (rango
    // no exacto): NUNCA sub-estimar (un vector de 32 B como 8 perderia el
    // solapamiento con offsets 8..31 -> dependencia de memoria perdida).
    switch (raw) {
    case 1: case 2: case 4: case 8:
    case 16: case 32: case 64: return raw;
    default: return 0;
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
    // Ancho VECTORIAL (16/32/64) para los ops VEC_*: vive en imm&0xFF (bytes).
    const int32_t vw = memory_access_size_bytes(static_cast<int32_t>(ins.imm & 0xFF));

    switch (ins.op) {
    // --- Lecturas con direccion precisa (offset+ancho) ---
    case Op::LOAD:
        if (!ops.empty()) {
            a.touches = a.is_load = true;
            a.reads.push_back(loc_of(pt, ops[0], w));
        }
        return a;
    // --- Lecturas de cabecera de objeto (whole-object) ---
    case Op::GETFIELD:
    case Op::ARRAY_LOAD:
    case Op::ARRAY_LEN:
        if (!ops.empty()) {
            a.touches = a.is_load = true;
            a.reads.push_back(loc_of(pt, ops[0], 0));
        }
        return a;
    // --- Escrituras con direccion precisa ---
    case Op::STORE:
        if (ops.size() >= 2) {
            a.touches = a.is_store = true;
            a.writes.push_back(loc_of(pt, ops[1], w));
        }
        return a;
    // --- Escrituras a campo/elemento (whole-object) ---
    case Op::SETFIELD:
    case Op::ARRAY_STORE:
    case Op::GCWB_IR:
        if (!ops.empty()) {
            a.touches = a.is_store = true;
            a.writes.push_back(loc_of(pt, ops[0], 0));
        }
        return a;
    // --- MEMCPY: NO es opaco.  dst=ops[0] (write), src=ops[1] (read).  El
    //     offset exacto no se conoce -> whole-root (disjuncion por RAiZ). ---
    case Op::MEMCPY:
        if (ops.size() >= 3) {
            a.touches = a.is_load = a.is_store = true;
            a.reads.push_back(loc_of(pt, ops[1], 0));  // src
            a.writes.push_back(loc_of(pt, ops[0], 0)); // dst
        } else {
            a.touches = a.opaque = a.is_store = true;
            a.writes.push_back(unknown_loc());
        }
        return a;

    // --- Ops VECTORIALES sobre PUNTEROS (SIMD 16/32/64 B): footprint PRECISO.
    //     El ancho (vw) es 16/32/64 de imm&0xFF; los operandos son punteros a
    //     memoria (base+i*esz).  NUNCA sub-estimar el rango vectorial. ---
    case Op::VEC_UNOP: // {dst, src}: escribe dst, lee src
        if (ops.size() >= 2) {
            a.touches = a.is_load = a.is_store = true;
            a.writes.push_back(loc_of(pt, ops[0], vw));
            a.reads.push_back(loc_of(pt, ops[1], vw));
        }
        return a;
    case Op::VEC_BINOP: // {dst, a, b}: escribe dst, lee a y b
        if (ops.size() >= 3) {
            a.touches = a.is_load = a.is_store = true;
            a.writes.push_back(loc_of(pt, ops[0], vw));
            a.reads.push_back(loc_of(pt, ops[1], vw));
            a.reads.push_back(loc_of(pt, ops[2], vw));
        }
        return a;
    case Op::VEC_BINOP_S: // {dst, a, ESCALAR}: escribe dst, lee a (op2 = escalar)
        if (ops.size() >= 2) {
            a.touches = a.is_load = a.is_store = true;
            a.writes.push_back(loc_of(pt, ops[0], vw));
            a.reads.push_back(loc_of(pt, ops[1], vw));
        }
        return a;
    case Op::VEC_FMA: // {c, d, a, b}: c = a*b + d -> escribe c, lee d/a/b
        if (ops.size() >= 4) {
            a.touches = a.is_load = a.is_store = true;
            a.writes.push_back(loc_of(pt, ops[0], vw));
            a.reads.push_back(loc_of(pt, ops[1], vw));
            a.reads.push_back(loc_of(pt, ops[2], vw));
            a.reads.push_back(loc_of(pt, ops[3], vw));
        }
        return a;
    // --- VEC_BCAST: broadcast de un ESCALAR a un registro vectorial -> NO toca
    //     memoria (el operando es un valor escalar, el destino es un reg). ---
    case Op::VEC_BCAST:
        return a; // touches = false

    // --- VEC_ACC_* (acumuladores de reduccion): acc_slot es una region host
    //     accedida a offsets complejos (acc_imm); modelarla con precision es
    //     intrincado -> OPACA (sound: aliasa conservador).  Menos comun
    //     (loops de reduccion).  Follow-up: modelar el offset del acumulador. ---
    case Op::VEC_ACC_ZERO:
    case Op::VEC_ACC_ADD:
    case Op::VEC_ACC_FMA:
    case Op::VEC_ACC_STORE:
    case Op::VEC_ACC_COMBINE:
        a.touches = a.opaque = a.is_load = a.is_store = true;
        return a;

    default:
        return a; // no es un acceso a memoria localizable
    }
}

} // namespace analysis
