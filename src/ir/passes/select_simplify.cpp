/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file select_simplify.cpp
 * @brief Implementacion de la canonicalizacion algebraica de SELECT.
 *
 * Ver @c include/ir/passes/select_simplify.h.  Cada regla reescribe el SELECT
 * in situ (mismo dst, cambia op/operandos), sin crear valores nuevos.
 */

#include "ir/passes/select_simplify.h"
#include "ir/ssa_ir.h"

#include <cstdint>
#include <unordered_map>

namespace ir {
namespace {

using DefIndex = std::unordered_map<IrValueId, const IrInstr *>;

DefIndex build_def_index(const IrFunction &fn) {
    DefIndex di;
    for (const auto &b : fn.blocks)
        for (const auto &ins : b.instrs)
            if (ins.dst != IR_NO_VALUE) di[ins.dst] = &ins;
    return di;
}

const IrInstr *def_of(const DefIndex &di, IrValueId v) {
    auto it = di.find(v);
    return it == di.end() ? nullptr : it->second;
}

bool as_const(const IrFunction &fn, IrValueId vid, uint64_t &out) {
    if (vid == IR_NO_VALUE || static_cast<size_t>(vid) >= fn.values.size())
        return false;
    if (!fn.values[vid].is_const) return false;
    out = fn.values[vid].const_val;
    return true;
}

/// @brief Reescribe @p sel a `mov dst, src` (copia).  DCE/copy-prop la limpian.
void to_mov(IrInstr &sel, IrValueId src) {
    sel.op = IrOp::MOV;
    sel.operands.clear();
    sel.operands.push_back(src);
    sel.phi_args.clear();
}

/// @brief true si @p op es una comparacion entera; rellena si es "menor" y si
///        es sin signo.
bool cmp_info(IrOp op, bool &less, bool &uns) {
    switch (op) {
    case IrOp::CMP_LT:
        less = true;
        uns = false;
        return true;
    case IrOp::CMP_LE:
        less = true;
        uns = false;
        return true;
    case IrOp::CMP_GT:
        less = false;
        uns = false;
        return true;
    case IrOp::CMP_GE:
        less = false;
        uns = false;
        return true;
    case IrOp::CMP_ULT:
        less = true;
        uns = true;
        return true;
    case IrOp::CMP_ULE:
        less = true;
        uns = true;
        return true;
    case IrOp::CMP_UGT:
        less = false;
        uns = true;
        return true;
    case IrOp::CMP_UGE:
        less = false;
        uns = true;
        return true;
    default: return false;
    }
}

/**
 * @brief Intenta simplificar UN select (in situ).  @return true si cambio.
 */
bool simplify_one(IrFunction &fn, const DefIndex &di, IrInstr &sel) {
    if (sel.op != IrOp::SELECT || sel.operands.size() != 3) return false;
    const IrValueId cond = sel.operands[0];
    const IrValueId a = sel.operands[1]; // valor si cond
    const IrValueId b = sel.operands[2]; // valor si !cond

    // (1) select(c, x, x) -> x
    if (a == b) {
        to_mov(sel, a);
        return true;
    }

    // (2) cond constante -> rama fija
    uint64_t cv = 0;
    if (as_const(fn, cond, cv)) {
        to_mov(sel, cv != 0 ? a : b);
        return true;
    }

    // (3) select(c, 1, 0) -> zext(c)  (c ya es 0/1; zext limpia bits altos)
    uint64_t av = 0, bv = 0;
    if (as_const(fn, a, av) && as_const(fn, b, bv) && av == 1 && bv == 0) {
        sel.op = IrOp::ZEXT;
        sel.operands.clear();
        sel.operands.push_back(cond);
        sel.phi_args.clear();
        return true;
    }

    // (5) select(!c, a, b) -> select(c, b, a)   (!c = xor c, 1)
    if (const IrInstr *cd = def_of(di, cond)) {
        if (cd->op == IrOp::XOR && cd->operands.size() == 2) {
            uint64_t k = 0;
            IrValueId inner = IR_NO_VALUE;
            if (as_const(fn, cd->operands[1], k) && k == 1)
                inner = cd->operands[0];
            else if (as_const(fn, cd->operands[0], k) && k == 1)
                inner = cd->operands[1];
            if (inner != IR_NO_VALUE) {
                sel.operands[0] = inner;
                sel.operands[1] = b;
                sel.operands[2] = a;
                return true;
            }
        }

        // (6) select(cmp(x,y), a, b) con {x,y}=={a,b} -> imin/imax(a,b)
        bool less = false, uns = false;
        if (cmp_info(cd->op, less, uns) && cd->operands.size() == 2) {
            const IrValueId cx = cd->operands[0], cy = cd->operands[1];
            const bool same_set = (a == cx && b == cy) || (a == cy && b == cx);
            if (same_set && cx != cy) {
                // cond true (cx REL cy) selecciona a.
                //  less:  a==cx -> min ; a==cy -> max
                //  greater: a==cx -> max ; a==cy -> min
                const bool is_min = less ? (a == cx) : (a == cy);
                sel.op = is_min ? (uns ? IrOp::IMINU : IrOp::IMIN)
                                : (uns ? IrOp::IMAXU : IrOp::IMAX);
                sel.operands.clear();
                sel.operands.push_back(a);
                sel.operands.push_back(b);
                sel.phi_args.clear();
                return true;
            }
        }
    }

    // (7) anidado con la MISMA cond:
    //   select(c, select(c, a2, b2), d) -> select(c, a2, d)
    //   select(c, a, select(c, b2, d2)) -> select(c, a, d2)
    if (const IrInstr *ad = def_of(di, a)) {
        if (ad->op == IrOp::SELECT && ad->operands.size() == 3 &&
            ad->operands[0] == cond) {
            sel.operands[1] = ad->operands[1]; // a2 (rama true anidada)
            return true;
        }
    }
    if (const IrInstr *bd = def_of(di, b)) {
        if (bd->op == IrOp::SELECT && bd->operands.size() == 3 &&
            bd->operands[0] == cond) {
            sel.operands[2] = bd->operands[2]; // d2 (rama false anidada)
            return true;
        }
    }

    return false;
}

} // namespace

int ir_pass_select_simplify(IrFunction &fn) {
    int total = 0;
    bool changed = true;
    // Punto fijo: una simplificacion (p.ej. colapsar un anidado) puede
    // habilitar otra.  Se reconstruye el def-index cada pasada porque los ops
    // cambian.
    while (changed) {
        changed = false;
        const DefIndex di = build_def_index(fn);
        for (auto &b : fn.blocks)
            for (auto &ins : b.instrs)
                if (simplify_one(fn, di, ins)) {
                    ++total;
                    changed = true;
                }
    }
    return total;
}

} // namespace ir
