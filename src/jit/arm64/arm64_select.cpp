/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file arm64_select.cpp
 * @brief Implementacion del selector IR -> AArch64 (template slot-por-valor).
 */

#include "jit/arm64/arm64_select.h"

#include <sstream>

namespace jit {
namespace arm64 {

namespace {

/// Offset del hueco de pila del valor SSA @p v (8 bytes por valor).
uint32_t slot_off(ir::IrValueId v) { return 8u * static_cast<uint32_t>(v); }

/// Emite la carga de un inmediato de 64 bits en @p reg con movz/movk.
void emit_imm(std::ostringstream &os, const char *reg, uint64_t imm) {
    os << "    movz " << reg << ", #" << (imm & 0xFFFF) << "\n";
    for (int shift = 16; shift < 64; shift += 16) {
        uint64_t chunk = (imm >> shift) & 0xFFFF;
        if (chunk != 0)
            os << "    movk " << reg << ", #" << chunk << ", lsl #" << shift
               << "\n";
    }
}

/// Carga el valor SSA @p v del hueco de pila a @p reg.
void emit_ld(std::ostringstream &os, const char *reg, ir::IrValueId v) {
    os << "    ldr " << reg << ", [sp, #" << slot_off(v) << "]\n";
}

/// Guarda @p reg en el hueco de pila del valor SSA @p v.
void emit_st(std::ostringstream &os, const char *reg, ir::IrValueId v) {
    os << "    str " << reg << ", [sp, #" << slot_off(v) << "]\n";
}

/// Mnemonico AArch64 de una op binaria entera, o nullptr si no soportada.
const char *binop_mnem(ir::IrOp op) {
    switch (op) {
    case ir::IrOp::ADD: return "add";
    case ir::IrOp::SUB: return "sub";
    case ir::IrOp::MUL: return "mul";
    case ir::IrOp::AND: return "and";
    case ir::IrOp::OR: return "orr";
    case ir::IrOp::XOR: return "eor";
    case ir::IrOp::SHL: return "lsl";
    case ir::IrOp::SHR: return "lsr";
    default: return nullptr;
    }
}

} // namespace

std::string arm64_emit_asm(const ir::IrFunction &fn, bool &out_unsupported) {
    out_unsupported = false;

    // Bootstrap H.2a: un solo bloque de linea recta.
    if (fn.blocks.size() != 1) {
        out_unsupported = true;
        return "";
    }

    // Marco de pila: 8 bytes por valor SSA, alineado a 16.
    uint32_t frame = static_cast<uint32_t>(fn.values.size()) * 8u;
    frame = (frame + 15u) & ~15u;
    if (frame == 0)
        frame = 16;

    std::ostringstream os;
    // Prologo: reservar el marco.
    os << "    sub sp, sp, #" << frame << "\n";
    // Guardar los params (AAPCS64: x0..x7) en sus huecos.
    const char *arg_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
    for (size_t i = 0; i < fn.params.size() && i < 8; ++i)
        emit_st(os, arg_regs[i], fn.params[i]);

    const ir::IrBlock &bb = fn.blocks[0];
    for (const ir::IrInstr &in : bb.instrs) {
        switch (in.op) {
        case ir::IrOp::CONST:
            emit_imm(os, "x9", in.imm);
            emit_st(os, "x9", in.dst);
            break;
        case ir::IrOp::MOV:
            if (in.operands.size() != 1) {
                out_unsupported = true;
                return "";
            }
            emit_ld(os, "x9", in.operands[0]);
            emit_st(os, "x9", in.dst);
            break;
        case ir::IrOp::NEG:
        case ir::IrOp::NOT: {
            if (in.operands.size() != 1) {
                out_unsupported = true;
                return "";
            }
            emit_ld(os, "x9", in.operands[0]);
            os << (in.op == ir::IrOp::NEG ? "    neg x9, x9\n"
                                          : "    mvn x9, x9\n");
            emit_st(os, "x9", in.dst);
            break;
        }
        case ir::IrOp::ADD:
        case ir::IrOp::SUB:
        case ir::IrOp::MUL:
        case ir::IrOp::AND:
        case ir::IrOp::OR:
        case ir::IrOp::XOR:
        case ir::IrOp::SHL:
        case ir::IrOp::SHR: {
            if (in.operands.size() != 2) {
                out_unsupported = true;
                return "";
            }
            const char *m = binop_mnem(in.op);
            emit_ld(os, "x9", in.operands[0]);
            emit_ld(os, "x10", in.operands[1]);
            os << "    " << m << " x9, x9, x10\n";
            emit_st(os, "x9", in.dst);
            break;
        }
        case ir::IrOp::RET:
            if (!in.operands.empty())
                emit_ld(os, "x0", in.operands[0]);
            os << "    add sp, sp, #" << frame << "\n";
            os << "    ret\n";
            break;
        default:
            // Op no soportada aun (rama, float, call, memoria): H.2b+.
            out_unsupported = true;
            return "";
        }
    }
    return os.str();
}

} // namespace arm64
} // namespace jit
