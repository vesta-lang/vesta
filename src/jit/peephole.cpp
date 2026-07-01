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

/// Desactiva SOLO el idiom xor-zeroing (bisection), dejando el resto activo.
bool xorzero_disabled() noexcept {
    static const bool off = [] {
        const char *v = std::getenv("VESTA_NO_XORZERO");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return off;
}

/// True si @p op LEE los flags (su resultado depende de RFLAGS).  Solo
/// Jcc/SETcc/CMOVcc en el set actual (ADC/SBB/RCL/RCR no existen todavia).
bool mop_reads_flags(MOp op) noexcept {
    switch (op) {
    case MOp::JCC:
    case MOp::SETCC:
    case MOp::CMOVCC:
        return true;
    default:
        return false;
    }
}

/// True si @p op sobrescribe el conjunto COMPLETO de flags (CF/ZF/SF/OF/PF)
/// de forma incondicional -> mata cualquier valor previo.  Conservador: solo
/// las que garantizan escribir todos.  INC/DEC (no tocan CF), NOT (no toca
/// flags), ROL/ROR (solo CF/OF), IMUL/POPCNT/LZCNT/TZCNT (ZF parcial/undef) y
/// UCOMISD se tratan como NEUTRALES (se sigue escaneando) -> solo hace el
/// analisis MAS conservador, nunca menos, asi que es seguro.
bool mop_kills_all_flags(MOp op) noexcept {
    switch (op) {
    case MOp::ADD:
    case MOp::SUB:
    case MOp::CMP:
    case MOp::TEST:
    case MOp::AND:
    case MOp::OR:
    case MOp::XOR:
    case MOp::NEG:
    case MOp::SHL:
    case MOp::SHR:
    case MOp::SAR:
    case MOp::CALL:
    case MOp::CALL_ABS:
    case MOp::SAFEPOINT: /* expande a cmp byte[rbx],0 + jne -> clobbea flags */
        return true;
    default:
        return false;
    }
}

/// True si @p in es un `mov reg, 0` (materializacion de cero en un GP).  Un
/// MOV con fuente inmediata siempre destina a un GP (no existe `mov xmm,imm`),
/// asi que no hay riesgo de tocar el banco flotante.
bool is_zero_mov(const MInstr &in) noexcept {
    return in.op == MOp::MOV && in.dst.kind == MOperandKind::REG &&
           in.src1.kind == MOperandKind::IMM32 && in.src1.value == 0 &&
           in.src2.kind == MOperandKind::NONE;
}

/// True si los flags estan MUERTOS justo despues del indice @p i del bloque
/// @p b: el primer uso relevante hacia adelante es un ESCRITOR completo (o se
/// alcanza el fin del bloque sin lectores, en cuyo caso -conservador- se
/// considera VIVO).  Habilita sustituir `mov reg,0` por `xor reg,reg` (que SI
/// escribe flags) sin corromper a un consumidor de flags posterior.
bool flags_dead_after(const MBlock &b, size_t i) noexcept {
    for (size_t j = i + 1; j < b.instrs.size(); ++j) {
        const MOp op = b.instrs[j].op;
        if (mop_reads_flags(op)) return false;    // lector primero -> VIVO
        if (mop_kills_all_flags(op)) return true; // escritor total -> MUERTO
        /* neutral -> seguir escaneando */
    }
    return false; // fin de bloque sin resolver -> conservador: VIVO
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
    const bool no_xz = xorzero_disabled();
    uint32_t removed = 0;
    for (MBlock &b : pf.blocks) {
        std::vector<MInstr> kept;
        kept.reserve(b.instrs.size());
        bool changed = false;
        for (size_t i = 0; i < b.instrs.size(); ++i) {
            const MInstr &in = b.instrs[i];
            if (is_redundant_self_move(in)) {
                ++removed;
                changed = true;
                continue;
            }
            /* xor-zeroing: `mov reg,0` -> `xor reg,reg` (2-3 bytes vs 5-7,
             * dependency-breaking idiom reconocido por la CPU) SOLO cuando los
             * flags esten muertos en ese punto (xor los escribe, mov no). */
            if (!no_xz && is_zero_mov(in) && flags_dead_after(b, i)) {
                const MOperand r =
                    MOperand::make_reg(static_cast<MReg>(in.dst.reg), 8);
                kept.push_back(MInstr::make_unary(MOp::XOR, r, r));
                changed = true;
                continue;
            }
            kept.push_back(in);
        }
        if (changed) b.instrs = std::move(kept);
    }
    return removed;
}

} // namespace jit
