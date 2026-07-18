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

#include <cctype>
#include <sstream>
#include <string>

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

/// Codigo de condicion AArch64 (cset/b.cc) de una op CMP, o nullptr si no lo es.
const char *cmp_cc(ir::IrOp op) {
    switch (op) {
    case ir::IrOp::CMP_EQ: return "eq";
    case ir::IrOp::CMP_NE: return "ne";
    case ir::IrOp::CMP_LT: return "lt";
    case ir::IrOp::CMP_GT: return "gt";
    case ir::IrOp::CMP_LE: return "le";
    case ir::IrOp::CMP_GE: return "ge";
    case ir::IrOp::CMP_ULT: return "lo";
    case ir::IrOp::CMP_UGT: return "hi";
    case ir::IrOp::CMP_ULE: return "ls";
    case ir::IrOp::CMP_UGE: return "hs";
    default: return nullptr;
    }
}

/// Emite las copias de los PHI del bloque @p to que llegan desde @p from: por
/// cada @c phi cuyo arg venga de @p from, copia arg -> dst (via x11).  Asi el
/// modelo slot-por-valor resuelve los PHI en los predecesores (como el x86).
void emit_phi_copies(std::ostringstream &os, const ir::IrFunction &fn,
                     ir::IrBlockId from, ir::IrBlockId to) {
    if (to >= fn.blocks.size())
        return;
    for (const ir::IrInstr &in : fn.blocks[to].instrs) {
        if (in.op != ir::IrOp::PHI)
            continue;
        for (const ir::IrPhiArg &pa : in.phi_args)
            if (pa.block == from) {
                emit_ld(os, "x11", pa.value);
                emit_st(os, "x11", in.dst);
            }
    }
}

} // namespace

std::string arm64_emit_asm(const ir::IrFunction &fn, bool &out_unsupported,
                           Arm64Abi abi) {
    out_unsupported = false;
    (void)abi; // el subconjunto entero no variadico es comun a las 3 ABIs.
    if (fn.blocks.empty()) {
        out_unsupported = true;
        return "";
    }

    // ¿La funcion hace alguna llamada?  Si la hace, `bl` machaca LR (x30): hay
    // que salvarlo en el prologo y restaurarlo antes de cada ret.
    bool has_call = false;
    for (const ir::IrBlock &bb : fn.blocks)
        for (const ir::IrInstr &in : bb.instrs)
            if (in.op == ir::IrOp::CALL)
                has_call = true;

    // Marco: area de valores (8B por valor SSA, alineada a 16) + 16B para LR si
    // hay llamadas (usa 8, 8 de padding para mantener sp a 16).
    uint32_t value_area = static_cast<uint32_t>(fn.values.size()) * 8u;
    value_area = (value_area + 15u) & ~15u;
    const uint32_t lr_off = value_area;
    uint32_t frame = value_area + (has_call ? 16u : 0u);
    if (frame == 0)
        frame = 16;

    // Prefijo de etiquetas locales por-funcion (saneado): evita colisiones
    // cuando varias funciones se ensamblan en el mismo unit (p.ej. un caller y su
    // callee).  Cada bloque -> <pfx>b<id>, cada rama -> <pfx>t<n>.
    std::string lbl = ".L";
    for (char c : fn.name)
        lbl += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    lbl += "_";

    std::ostringstream os;
    // Prologo: reservar el marco, salvar LR si hace falta, y guardar los params
    // (AAPCS64: x0..x7).  Cae al bloque 0 (los saltos van a <pfx>b<id>, despues).
    os << "    sub sp, sp, #" << frame << "\n";
    if (has_call)
        os << "    str x30, [sp, #" << lr_off << "]\n";
    const char *arg_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
    for (size_t i = 0; i < fn.params.size() && i < 8; ++i)
        emit_st(os, arg_regs[i], fn.params[i]);

    int cond_lbl = 0;   // contador de etiquetas locales de BR_COND.
    int atomic_lbl = 0; // contador de etiquetas de los bucles LL/SC atomicos.
    for (const ir::IrBlock &bb : fn.blocks) {
        os << lbl << "b" << bb.id << ":\n";
        for (const ir::IrInstr &in : bb.instrs) {
            switch (in.op) {
            case ir::IrOp::PHI:
                break; // resuelto por copias en los predecesores.
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
                emit_ld(os, "x9", in.operands[0]);
                emit_ld(os, "x10", in.operands[1]);
                os << "    " << binop_mnem(in.op) << " x9, x9, x10\n";
                emit_st(os, "x9", in.dst);
                break;
            }
            case ir::IrOp::CMP_EQ:
            case ir::IrOp::CMP_NE:
            case ir::IrOp::CMP_LT:
            case ir::IrOp::CMP_GT:
            case ir::IrOp::CMP_LE:
            case ir::IrOp::CMP_GE:
            case ir::IrOp::CMP_ULT:
            case ir::IrOp::CMP_UGT:
            case ir::IrOp::CMP_ULE:
            case ir::IrOp::CMP_UGE: {
                if (in.operands.size() != 2) {
                    out_unsupported = true;
                    return "";
                }
                emit_ld(os, "x9", in.operands[0]);
                emit_ld(os, "x10", in.operands[1]);
                os << "    cmp x9, x10\n";
                os << "    cset x9, " << cmp_cc(in.op) << "\n";
                emit_st(os, "x9", in.dst);
                break;
            }
            case ir::IrOp::BR:
                // Copias de PHI del destino, luego el salto incondicional.
                emit_phi_copies(os, fn, bb.id, in.target_block);
                os << "    b " << lbl << "b" << in.target_block << "\n";
                break;
            case ir::IrOp::BR_COND: {
                if (in.operands.size() != 1) {
                    out_unsupported = true;
                    return "";
                }
                // cond != 0 -> rama true; cada arista lleva SUS copias de PHI.
                const int n = cond_lbl++;
                emit_ld(os, "x9", in.operands[0]);
                os << "    cbnz x9, " << lbl << "t" << n << "\n";
                emit_phi_copies(os, fn, bb.id, in.false_block);
                os << "    b " << lbl << "b" << in.false_block << "\n";
                os << lbl << "t" << n << ":\n";
                emit_phi_copies(os, fn, bb.id, in.target_block);
                os << "    b " << lbl << "b" << in.target_block << "\n";
                break;
            }
            case ir::IrOp::ATOMIC_CAS_I64: {
                // compare-and-swap via bucle load-linked/store-conditional.
                // operands: addr, exp, des -> dst = valor viejo.
                if (in.operands.size() != 3 || in.dst == ir::IR_NO_VALUE) {
                    out_unsupported = true;
                    return "";
                }
                const int n = atomic_lbl++;
                emit_ld(os, "x9", in.operands[0]);  // addr
                emit_ld(os, "x10", in.operands[1]); // exp
                emit_ld(os, "x11", in.operands[2]); // des
                os << lbl << "acr" << n << ":\n";
                os << "    ldaxr x12, [x9]\n";      // old (load-acquire)
                os << "    cmp x12, x10\n";
                os << "    b.ne " << lbl << "acd" << n << "\n"; // mismatch: no store
                os << "    stlxr w13, x11, [x9]\n"; // store-release; w13=0 ok
                os << "    cbnz w13, " << lbl << "acr" << n << "\n"; // reintenta
                os << lbl << "acd" << n << ":\n";
                emit_st(os, "x12", in.dst); // valor viejo (== o != exp)
                break;
            }
            case ir::IrOp::ATOMIC_ADD_I64: {
                // fetch-and-add via bucle LL/SC.  operands: addr, delta -> dst =
                // valor viejo.
                if (in.operands.size() != 2 || in.dst == ir::IR_NO_VALUE) {
                    out_unsupported = true;
                    return "";
                }
                const int n = atomic_lbl++;
                emit_ld(os, "x9", in.operands[0]);  // addr
                emit_ld(os, "x10", in.operands[1]); // delta
                os << lbl << "aar" << n << ":\n";
                os << "    ldaxr x11, [x9]\n";       // old
                os << "    add x12, x11, x10\n";     // new = old + delta
                os << "    stlxr w13, x12, [x9]\n";  // store-release
                os << "    cbnz w13, " << lbl << "aar" << n << "\n";
                emit_st(os, "x11", in.dst); // valor viejo
                break;
            }
            case ir::IrOp::CALL: {
                // Args a x0..x7 desde sus huecos; bl; resultado (x0) al hueco.
                if (in.operands.size() > 8) {
                    out_unsupported = true;
                    return "";
                }
                for (size_t i = 0; i < in.operands.size(); ++i)
                    emit_ld(os, arg_regs[i], in.operands[i]);
                os << "    bl " << in.func_name << "\n";
                if (in.dst != ir::IR_NO_VALUE)
                    emit_st(os, "x0", in.dst);
                break;
            }
            case ir::IrOp::RET:
                if (!in.operands.empty())
                    emit_ld(os, "x0", in.operands[0]);
                if (has_call)
                    os << "    ldr x30, [sp, #" << lr_off << "]\n";
                os << "    add sp, sp, #" << frame << "\n";
                os << "    ret\n";
                break;
            default:
                // Op no soportada aun (float, memoria, dispatch dinamico): H.3+.
                out_unsupported = true;
                return "";
            }
        }
    }
    return os.str();
}

} // namespace arm64
} // namespace jit
