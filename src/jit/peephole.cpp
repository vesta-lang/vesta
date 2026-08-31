/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/peephole.cpp
 * @brief Implementacion del peephole sobre MachineIR fisico (ver peephole.h).
 */

#include "util/env_flags.h"
#include "jit/peephole.h"
#include "analysis/facts/phys_liveness.h" // diagnostico: ver el hecho en accion

#include <cstdio>
#include <string>

#include <cstdlib>
#include <vector>

namespace jit {

namespace {

bool peephole_disabled() noexcept {
    static const bool off = util::flag_on(util::FlagId::NoPeephole);
    return off;
}

/// Solo DICE que escrituras borraria la regla que no esta puesta, sin borrar
/// nada.  Es el instrumento para cerrarla: cada linea que imprime sobre un
/// programa que funciona es una escritura que el hecho da por muerta, y basta
/// con mirar UNA que no lo este para saber que fuente de liveness falta.
///
/// Filtra por NOMBRE de funcion, como @c VESTA_VREG_DUMP: sin filtro esto
/// imprime miles de lineas y no se lee.
bool deaddef_report(const std::string &fn) noexcept {
    const std::string &want = util::flag_text(util::FlagId::DeadDefReport);
    return !want.empty() && fn.find(want) != std::string::npos;
}

/// Desactiva SOLO el idiom xor-zeroing (bisection), dejando el resto activo.
bool xorzero_disabled() noexcept {
    static const bool off = util::flag_on(util::FlagId::NoXorZero);
    return off;
}

/// True si @p op LEE los flags (su resultado depende de RFLAGS).  Solo
/// Jcc/SETcc/CMOVcc en el set actual (ADC/SBB/RCL/RCR no existen todavia).
bool mop_reads_flags(MOp op) noexcept {
    switch (op) {
    case MOp::JCC:
    case MOp::SETCC:
    case MOp::CMOVCC: return true;
    default: return false;
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
    default: return false;
    }
}

/// True si @p in es un `cmp reg, 0`.  `test reg, reg` pone EXACTAMENTE los
/// mismos 5 flags (CF=OF=0, ZF/SF/PF de reg) y es mas corto -> sustitucion
/// siempre segura para cualquier Jcc/SETcc posterior.  El encoder fuerza REX.W
/// en ambos, asi que la comparacion es de 64-bit en los dos casos.
bool is_cmp_zero(const MInstr &in) noexcept {
    return in.op == MOp::CMP && in.dst.kind == MOperandKind::REG &&
           in.src1.kind == MOperandKind::IMM32 && in.src1.value == 0;
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

/// Desactiva SOLO la eliminacion de JMP-a-fallthrough (bisection).
bool jmpfall_disabled() noexcept {
    static const bool off = util::flag_on(util::FlagId::NoJmpFall);
    return off;
}

/// Si @p in es un `jmp LABEL` INCONDICIONAL (no jmp-reg ni jmp-sym), devuelve
/// true y escribe el label_id en @p out.  Solo los JMP con operando LABEL son
/// candidatos a eliminacion por fallthrough.
bool is_label_jmp(const MInstr &in, uint32_t &out) noexcept {
    if (in.op != MOp::JMP) return false;
    if (in.src1.kind != MOperandKind::LABEL) return false; // jmp-reg / jmp-sym
    out = static_cast<uint32_t>(in.src1.value);
    return true;
}

/// Label que DEFINE el bloque @p b en su primera instruccion real (saltando
/// COMMENT/NOP).  Devuelve true + label_id si esa primera instruccion es un
/// LABEL_DEF; el resto de instrucciones intermedias no cuenta (un JMP solo
/// cae en fallthrough si el destino es la etiqueta de ENTRADA del bloque
/// siguiente).
bool block_entry_label(const MBlock &b, uint32_t &out) noexcept {
    for (const MInstr &in : b.instrs) {
        if (in.op == MOp::COMMENT || in.op == MOp::NOP) continue;
        if (in.op == MOp::LABEL_DEF) {
            out = static_cast<uint32_t>(in.src1.value);
            return true;
        }
        return false; // primera instruccion real no es un label
    }
    return false; // bloque vacio
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
    /* NO hay aqui una regla que borre ESCRITURAS QUE NADIE LEE, y no por no
     * haberlo intentado: se probaron tres versiones y las tres rompieron algo.
     *
     * La ultima ya preguntaba a @c analysis::facts::compute_phys_liveness --
     * el hecho que dice que sigue vivo tras cada instruccion, sacado de la
     * base -- y aun asi fallaban seis casos del recolector.  El modelo de "que
     * hace VIVO a un registro" en este backend tiene mas fuentes de las que el
     * hecho conoce hoy; mientras falte una, la regla borra algo que hacia
     * falta.
     *
     * Lo que SI quedo de esos intentos, y vale por su cuenta: que un `ret` y
     * una llamada DIGAN los registros que leen por convencion (estaban solo
     * como "barrera", que basta para no reordenar y no para liveness -- una
     * funcion que devolvia 42 empezaba a devolver 0, y un bucle se comia toda
     * la memoria de la maquina), que un bloque de `asm` diga los registros que
     * lee su cuerpo, y el hecho con sus pruebas.
     *
     * COMO SE RETOMA, que es lo util: `VESTA_DEAD_DEF_REPORT=<parte del nombre
     * de una funcion>` imprime que escrituras BORRARIA sin borrar ninguna.
     * Cada linea sobre un programa que funciona es una escritura que el hecho
     * da por muerta; basta con mirar UNA que no lo este para saber que fuente
     * de liveness falta.  Asi salieron las tres que ya estan cerradas -- lo
     * que lee un `ret`, lo que lee una llamada por convencion, y sus propios
     * operandos cuando la llamada es indirecta --: en `252_gc_ref_field` el
     * informe paso de diez lineas a una.
     *
     * Lo que queda por encontrar no es poco: con la regla puesta, la suite
     * ENTERA se cae -- POO, herencia, interfaces, polimorfismo, colas --, todo
     * devolviendo 0.  El grupo del recolector solo, en cambio, pasa 22 de 23;
     * mirar un subconjunto engana. */
    if (deaddef_report(pf.name)) {
        const analysis::facts::PhysLivenessFacts vivos =
            analysis::facts::compute_phys_liveness(pf);
        for (size_t bi = 0; bi < pf.blocks.size(); ++bi) {
            const MBlock &b = pf.blocks[bi];
            for (size_t i = 0; i < b.instrs.size(); ++i) {
                const MInstr &in = b.instrs[i];
                if (in.op != MOp::MOV) continue;
                if (in.dst.kind != MOperandKind::REG || in.dst.width != 8)
                    continue;
                if (in.src2.kind != MOperandKind::NONE) continue;
                if (vivos.is_live_after(static_cast<uint32_t>(bi),
                                        static_cast<uint32_t>(i), in.dst.reg))
                    continue;
                std::fprintf(stderr,
                             "[deaddef] %s bloque %zu instr %zu: mov r%u <- "
                             "(fuente kind %d) parece MUERTA%s\n",
                             pf.name.c_str(), bi, i,
                             static_cast<unsigned>(in.dst.reg),
                             static_cast<int>(in.src1.kind),
                             vivos.opaco ? " [hay puntos sin saber]" : "");
            }
        }
    }
    for (size_t bi = 0; bi < pf.blocks.size(); ++bi) {
        MBlock &b = pf.blocks[bi];
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
            /* cmp reg,0 -> test reg,reg (mismos flags, mas corto). */
            if (is_cmp_zero(in)) {
                const MOperand r = MOperand::make_reg(
                    static_cast<MReg>(in.dst.reg), in.dst.width);
                kept.push_back(MInstr::make_unary(MOp::TEST, r, r));
                changed = true;
                continue;
            }
            kept.push_back(in);
        }
        if (changed) b.instrs = std::move(kept);
    }

    /* Eliminacion de JMP-a-fallthrough.  Los bridge-blocks del critical-edge
     * split suelen terminar con `jmp .L` cuando .L es justo el bloque
     * siguiente -> el salto es muerto (la caida natural va al mismo sitio). */
    if (!jmpfall_disabled()) {
        for (size_t bi = 0; bi < pf.blocks.size(); ++bi) {
            MBlock &b = pf.blocks[bi];
            /* (1) intra-bloque: `jmp L` seguido inmediatamente de `L:`. */
            std::vector<MInstr> kept;
            kept.reserve(b.instrs.size());
            bool changed = false;
            for (size_t i = 0; i < b.instrs.size(); ++i) {
                uint32_t tgt = 0;
                if (i + 1 < b.instrs.size() && is_label_jmp(b.instrs[i], tgt) &&
                    b.instrs[i + 1].op == MOp::LABEL_DEF &&
                    static_cast<uint32_t>(b.instrs[i + 1].src1.value) == tgt) {
                    ++removed;
                    changed = true;
                    continue; // el `L:` siguiente lo hace redundante
                }
                /* intra-bloque: `jcc X ; jmp Y ; X:` -> `j!cc Y ; X:`.  El
                 * `je X; jmp Y` que emiten los null/bool-checks con X contiguo:
                 * invertir la condicion (XOR 1 sobre el codigo x86) + soltar el
                 * jmp.  Semantica identica; seguro (no toca registros). */
                uint32_t jmp_tgt = 0;
                if (i + 2 < b.instrs.size() && b.instrs[i].op == MOp::JCC &&
                    b.instrs[i].src1.kind == MOperandKind::LABEL &&
                    b.instrs[i].variant != static_cast<uint8_t>(MCond::NONE) &&
                    is_label_jmp(b.instrs[i + 1], jmp_tgt) &&
                    b.instrs[i + 2].op == MOp::LABEL_DEF) {
                    const uint32_t jcc_tgt =
                        static_cast<uint32_t>(b.instrs[i].src1.value);
                    const uint32_t lbl =
                        static_cast<uint32_t>(b.instrs[i + 2].src1.value);
                    if (jcc_tgt == lbl && jmp_tgt != lbl) {
                        MInstr inv = b.instrs[i];
                        inv.variant ^= 1u;
                        inv.src1 = MOperand::make_label(jmp_tgt);
                        kept.push_back(inv); // j!cc Y
                        ++removed;
                        changed = true;
                        ++i; // saltar el jmp (i+1); el X: (i+2) se procesa
                             // normal
                        continue;
                    }
                }
                kept.push_back(b.instrs[i]);
            }
            if (changed) b.instrs = std::move(kept);

            /* (2) inter-bloque: ultima instr `jmp L` y el bloque SIGUIENTE
             * entra por `L:` -> fallthrough natural. */
            if (b.instrs.empty() || bi + 1 >= pf.blocks.size()) continue;
            uint32_t tgt = 0, next_lbl = 0;
            if (is_label_jmp(b.instrs.back(), tgt) &&
                block_entry_label(pf.blocks[bi + 1], next_lbl) &&
                next_lbl == tgt) {
                b.instrs.pop_back();
                ++removed;
                continue;
            }

            /* (3) `jcc X ; jmp Y` con X = entrada del bloque SIGUIENTE
             * (fallthrough) -> `j!cc Y` (cae natural a X, elimina el jmp).
             * El selector emite BR_COND como `jcc taken; jmp not-taken`; cuando
             * el bloque taken queda contiguo, la mitad es redundante.  Aparece
             * en CADA null-check / bool-test / comparacion-a-branch.  Invertir
             * la condicion es XOR 1 sobre el codigo x86 (E<->NE, L<->GE, ...).
             * Semantica identica: `if cc goto X(fallthrough) else goto Y`  ==
             * `if !cc goto Y else fall-through a X`.  Seguro (no toca regs). */
            if (b.instrs.size() >= 2) {
                MInstr &last = b.instrs.back();
                MInstr &prev = b.instrs[b.instrs.size() - 2];
                uint32_t jmp_tgt = 0, nl = 0;
                if (is_label_jmp(last, jmp_tgt) && prev.op == MOp::JCC &&
                    prev.src1.kind == MOperandKind::LABEL &&
                    prev.variant != static_cast<uint8_t>(MCond::NONE) &&
                    block_entry_label(pf.blocks[bi + 1], nl)) {
                    const uint32_t jcc_tgt =
                        static_cast<uint32_t>(prev.src1.value);
                    if (jcc_tgt == nl && jmp_tgt != nl) {
                        prev.variant ^= 1u; /* invertir condicion x86 */
                        prev.src1 = MOperand::make_label(jmp_tgt);
                        b.instrs.pop_back();
                        ++removed;
                    }
                }
            }
        }
    }
    return removed;
}

} // namespace jit
