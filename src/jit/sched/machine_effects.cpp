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
 * @file jit/sched/machine_effects.cpp
 * @brief Efectos de una MInstr LEIDOS DE LAS DBs generadas (no re-derivados a
 *        mano).  Para una instruccion real de la ISA, la forma de @c instr_db
 *        (rmask/wmask/memflags + flags r/w de cada operando) es la fuente de
 *        verdad; los registros implicitos (rax:rdx de div...) salen del regset de la DB.
 *        Solo los PSEUDOS propios de VestaVM (que no existen en ninguna ISA) se
 *        modelan aqui explicitamente.
 */

#include "jit/sched/machine_effects.h"



#include <string>

namespace jit {
namespace sched {

namespace {

using vx::instr_db::DbForm;
using vx::instr_db::DbOperand;
using vx::instr_db::Isa;
using vx::instr_db::ParsedOp;

/// Clave de registro uniforme de un operando REG/VREG (o UINT32_MAX si no lo es).
uint32_t reg_key(const MOperand &o) {
    if (o.kind == MOperandKind::REG) return o.reg;
    if (o.kind == MOperandKind::VREG)
        return MEffects::VREG_BASE + static_cast<uint32_t>(o.value);
    return UINT32_MAX;
}

void add(std::vector<uint32_t> &v, uint32_t k) {
    if (k == UINT32_MAX) return;
    for (uint32_t x : v)
        if (x == k) return;
    v.push_back(k);
}

/// Registros de DIRECCION de un operando MEM (base + index) -> siempre LEIDOS.
void add_mem_addr_reads(MEffects &e, const MOperand &o) {
    if (o.kind != MOperandKind::MEM) return;
    add(e.reads, o.reg); // base
    const uint8_t index = (o.width >> 2) & 0x3F;
    if (index != static_cast<uint8_t>(MReg::NONE)) add(e.reads, index);
}

/**
 * @brief Decodifica un @c register_set de la DB (p.ej. "AX", "DX", "RCX", "R8")
 *        al id de @c MReg GP x86, si nombra un registro CONCRETO.  Devuelve -1
 *        si es una CLASE (contiene '/', "GPR64", ...) o no es GP -> ese operando
 *        no es un registro implicito fijo.
 *
 * La DB usa el nombre de la FAMILIA (A/C/D/B/SP/BP/SI/DI/R8..R15) con el ancho
 * como campo aparte; para las dependencias importa el registro completo.
 */
int regset_to_mreg(const char *rs) {
    if (rs == nullptr || rs[0] == '\0' || rs[0] == '-') return -1;
    // Una clase (varios registros) lleva '/'.  No es un implicito fijo.
    for (const char *p = rs; *p; ++p)
        if (*p == '/') return -1;
    const std::string s(rs);
    // Prefijo de ancho opcional (R/E) + letra de familia.
    // Familias de una letra: A C D B (acumulador/contador/datos/base).
    struct Fam { const char *k; int reg; };
    static const Fam fam[] = {
        {"AX", 0},  {"EAX", 0}, {"RAX", 0}, {"AL", 0},  {"AH", 0},
        {"CX", 1},  {"ECX", 1}, {"RCX", 1}, {"CL", 1},  {"CH", 1},
        {"DX", 2},  {"EDX", 2}, {"RDX", 2}, {"DL", 2},  {"DH", 2},
        {"BX", 3},  {"EBX", 3}, {"RBX", 3}, {"BL", 3},  {"BH", 3},
        {"SP", 4},  {"ESP", 4}, {"RSP", 4}, {"SPL", 4},
        {"BP", 5},  {"EBP", 5}, {"RBP", 5}, {"BPL", 5},
        {"SI", 6},  {"ESI", 6}, {"RSI", 6}, {"SIL", 6},
        {"DI", 7},  {"EDI", 7}, {"RDI", 7}, {"DIL", 7},
    };
    for (const Fam &f : fam)
        if (s == f.k) return f.reg;
    // R8..R15 (con sufijos D/W/B): "R8", "R8D", "R8W", "R8B", ... hasta R15.
    if ((s[0] == 'R' || s[0] == 'r') && s.size() >= 2 && s[1] >= '8' &&
        s[1] <= '9') {
        int n = s[1] - '0';
        if (s.size() >= 3 && s[2] >= '0' && s[2] <= '5') n = 10 + (s[2] - '0');
        if (n >= 8 && n <= 15) return n;
    }
    return -1;
}

/// El MInstr referencia N operandos explicitos (dst + src1 + src2 no-vacios).
const MOperand &minstr_slot(const MInstr &mi, int slot) {
    return slot == 0 ? mi.dst : (slot == 1 ? mi.src1 : mi.src2);
}

/// Traduce un operando del MInstr a su @c ParsedOp (kind + ancho en bits) para
/// el matcher de la DB.
bool to_parsed(const MOperand &o, ParsedOp &out) {
    switch (o.kind) {
    case MOperandKind::REG:
    case MOperandKind::VREG:
        out.kind = vx::instr_db::OP_REG;
        out.width = static_cast<uint16_t>(o.width) * 8;
        return true;
    case MOperandKind::IMM32:
    case MOperandKind::IMM64_IDX:
        out.kind = vx::instr_db::OP_IMM;
        out.width = 0;
        return true;
    case MOperandKind::MEM:
        out.kind = vx::instr_db::OP_MEM;
        out.width = 0;
        return true;
    case MOperandKind::LABEL:
    case MOperandKind::REL_RT:
        out.kind = vx::instr_db::OP_RELBR;
        out.width = 0;
        return true;
    default:
        return false; // NONE
    }
}

/// Cuenta de operandos explicitos del MInstr (dst/src1/src2 no vacios).
int explicit_operand_count(const MInstr &mi) {
    int n = 0;
    for (int s = 0; s < 3; ++s)
        if (minstr_slot(mi, s).kind != MOperandKind::NONE) ++n;
    return n;
}

/**
 * @brief Efectos de una instruccion REAL leidos de la DB.  Devuelve false si el
 *        mnemonico no esta en la DB (el llamador cae al modelo pseudo/barrera).
 */
bool db_effects(const MInstr &mi, const char *mnem, Isa isa, MEffects &e) {
    // Operandos del MInstr -> ParsedOp para el matcher.
    std::vector<ParsedOp> ops;
    const int nexp = explicit_operand_count(mi);
    for (int s = 0; s < nexp; ++s) {
        ParsedOp p;
        if (to_parsed(minstr_slot(mi, s), p)) ops.push_back(p);
    }
    const int32_t fid = vx::instr_db::match(isa, mnem, ops);
    if (fid < 0) return false;

    const vx::instr_db::IsaData &db =
        (isa == Isa::X86) ? vx::instr_db::db_x86() : vx::instr_db::db_arm64();
    if (fid >= static_cast<int32_t>(db.form_count)) return false;
    const DbForm &f = db.forms[static_cast<uint32_t>(fid)];

    // Flags: memflags bit2 = escribe flags, bit3 = lee flags.
    e.writes_flags = (f.memflags & 0x4) != 0;
    e.reads_flags = (f.memflags & 0x8) != 0;

    // Recorre los operandos de la FORMA.  Los explicitos (no implicit, no flags)
    // se emparejan por posicion con los slots del MInstr; su r/w viene de la DB.
    int slot = 0;
    for (uint8_t i = 0; i < f.ops_count; ++i) {
        const DbOperand &o = db.ops[f.ops_off + i];
        const bool rd = (o.flags & 0x1) != 0;
        const bool wr = (o.flags & 0x2) != 0;
        const bool implicit = (o.flags & 0x4) != 0;
        if (o.kind == vx::instr_db::OP_FLAGS) continue; // ya cubierto por memflags

        // Operando IMPLICITO de registro fijo (rax:rdx de div, rax de cmpxchg,
        // rsi/rdi/rcx de movs...): su identidad viene de la DB (regset), no de
        // una tabla a mano.  No consume un slot del MInstr.
        if (implicit) {
            if (o.kind == vx::instr_db::OP_REG && o.regset < db.str_count) {
                const int r = regset_to_mreg(db.str[o.regset]);
                if (r >= 0) {
                    if (rd) add(e.reads, static_cast<uint32_t>(r));
                    if (wr) add(e.writes, static_cast<uint32_t>(r));
                }
            } else if (o.kind == vx::instr_db::OP_MEM) {
                if (rd) e.reads_mem = true;
                if (wr) e.writes_mem = true;
            }
            continue;
        }

        if (slot >= 3) { ++slot; continue; }
        const MOperand &mo = minstr_slot(mi, slot);
        ++slot;
        if (mo.kind == MOperandKind::MEM) {
            add_mem_addr_reads(e, mo);
            if (rd) e.reads_mem = true;
            if (wr) e.writes_mem = true;
        } else {
            if (rd) add(e.reads, reg_key(mo));
            if (wr) add(e.writes, reg_key(mo));
        }
    }

    return true;
}

/**
 * @brief Modela los efectos de un PSEUDO de VestaVM (no existe en la ISA).
 *        Cada pseudo tiene una semantica fija y conocida por construccion.
 */
void pseudo_effects(const MInstr &mi, MEffects &e) {
    switch (mi.op) {
    /* Sin efecto de datos. */
    case MOp::NOP:
    case MOp::LABEL_DEF:
    case MOp::COMMENT:
    case MOp::DATA_PTR_LABEL:
    case MOp::DATA_REL32_LABEL:
        break;

    /* ARG: marca un argumento -> LEE su src1 (para no adelantar al productor). */
    case MOp::ARG:
        add(e.reads, reg_key(mi.src1));
        break;

    /* Cargas pseudo: dst = [addr]. */
    case MOp::LOAD:      // dst, src1=addr
    case MOp::LOAD_VM:   // dst, src1=addr, src2=imm64_idx (fallback)
        add(e.writes, reg_key(mi.dst));
        add(e.reads, reg_key(mi.src1));
        e.reads_mem = true;
        break;
    /* Stores pseudo: [addr] = val. */
    case MOp::STORE:     // src1=addr, src2=val
    case MOp::STORE_VM:  // src1=addr, src2=val, dst=imm64_idx
        add(e.reads, reg_key(mi.src1));
        add(e.reads, reg_key(mi.src2));
        e.writes_mem = true;
        break;

    /* ALLOCA: dst = puntero a espacio reservado del frame. */
    case MOp::ALLOCA:
    case MOp::ALLOCA_VM:
        add(e.writes, reg_key(mi.dst));
        break;

    /* Division/modulo en vregs (pre-rewrite): dst = src1 op src2; clobbea flags
     * y (al bajar) RAX/RDX -> se declara ese clobber para que un uso posterior
     * de RAX/RDX dependa de esta op. */
    case MOp::DIVMOD_V:
        add(e.writes, reg_key(mi.dst));
        add(e.reads, reg_key(mi.src1));
        add(e.reads, reg_key(mi.src2));
        add(e.writes, static_cast<uint8_t>(MReg::RAX));
        add(e.writes, static_cast<uint8_t>(MReg::RDX));
        e.writes_flags = true;
        break;

    /* Atomicos en vregs: RMW sobre memoria + flags + (al bajar) RAX. */
    case MOp::ATOMICCAS_V: // dst in/out (expected->old), src1=addr, src2=desired
        add(e.reads, reg_key(mi.dst));
        add(e.writes, reg_key(mi.dst));
        add(e.reads, reg_key(mi.src1));
        add(e.reads, reg_key(mi.src2));
        add(e.writes, static_cast<uint8_t>(MReg::RAX));
        e.reads_mem = true;
        e.writes_mem = true;
        e.writes_flags = true;
        break;
    case MOp::ATOMICADD_V: // dst=old, src1=addr, src2=delta
        add(e.writes, reg_key(mi.dst));
        add(e.reads, reg_key(mi.src1));
        add(e.reads, reg_key(mi.src2));
        e.reads_mem = true;
        e.writes_mem = true;
        e.writes_flags = true;
        break;
    /* Atomicos fisicos (post-rewrite): dst=mem[addr], src1=reg. */
    case MOp::LOCK_CMPXCHG:
        add(e.reads, reg_key(mi.src1));
        add(e.reads, static_cast<uint8_t>(MReg::RAX));
        add(e.writes, static_cast<uint8_t>(MReg::RAX));
        add_mem_addr_reads(e, mi.dst);
        e.reads_mem = true;
        e.writes_mem = true;
        e.writes_flags = true;
        break;
    case MOp::LOCK_XADD:
        add(e.reads, reg_key(mi.src1));
        add(e.writes, reg_key(mi.src1)); // xadd deja el valor viejo en el reg
        add_mem_addr_reads(e, mi.dst);
        e.reads_mem = true;
        e.writes_mem = true;
        e.writes_flags = true;
        break;

    /* memcpy nativo: opera sobre RSI/RDI/RCX + memoria. */
    case MOp::REP_MOVSB:
        add(e.reads, static_cast<uint8_t>(MReg::RSI));
        add(e.reads, static_cast<uint8_t>(MReg::RDI));
        add(e.reads, static_cast<uint8_t>(MReg::RCX));
        add(e.writes, static_cast<uint8_t>(MReg::RSI));
        add(e.writes, static_cast<uint8_t>(MReg::RDI));
        add(e.writes, static_cast<uint8_t>(MReg::RCX));
        e.reads_mem = true;
        e.writes_mem = true;
        break;

    /* Carga del ProcessVM*: dst (RBX) = puntero; puede llamar al runtime. */
    case MOp::LOAD_PROC:
        add(e.writes, reg_key(mi.dst));
        e.reads_mem = true; // TLS-direct: gs:[...]
        break;

    /* Direcciones de simbolo/label/TLS: dst = &X (sin flags, sin memoria). */
    case MOp::MOV_SYM:
    case MOp::LEA_RIP_SYM:
    case MOp::LEA_LABEL:
    case MOp::TLS_LE_ADDR:
    case MOp::TLS_PE_ADDR:
        add(e.writes, reg_key(mi.dst));
        break;

    /* Salva/restaura proc->registers en la work-area del frame (usa R11). */
    case MOp::CB_SAVE_REGS:
        add(e.writes, static_cast<uint8_t>(MReg::R11));
        e.reads_mem = true;
        e.writes_mem = true;
        break;
    case MOp::CB_RESTORE_REGS:
        add(e.writes, static_cast<uint8_t>(MReg::R11));
        e.reads_mem = true;
        e.writes_mem = true;
        break;

    /* Barreras: control de flujo / puntos de sincronizacion / asm opaco. */
    case MOp::JMP:
        e.is_barrier = true;
        break;
    case MOp::JCC:
        e.reads_flags = true;
        e.is_barrier = true;
        break;
    case MOp::CALL:
    case MOp::CALL_ABS:
    case MOp::CALL_SYM:
    case MOp::JMP_SYM:
    case MOp::TAILCALL:
    case MOp::RET:
    case MOp::SAFEPOINT:
    case MOp::INT3:
    case MOp::INLINE_ASM_RAW:
        e.is_barrier = true;
        break;

    /* Cualquier otro pseudo no listado: barrera dura (nunca miscompilar). */
    default:
        add(e.reads, reg_key(mi.dst));
        add(e.reads, reg_key(mi.src1));
        add(e.reads, reg_key(mi.src2));
        e.reads_mem = true;
        e.writes_mem = true;
        e.writes_flags = true;
        e.reads_flags = true;
        e.is_barrier = true;
        break;
    }
}

} // namespace

const char *mop_mnemonic(MOp op, EffIsa isa) {
    if (isa != EffIsa::X86) return nullptr; // arm64 entra con el pipeline vreg arm

    switch (op) {
    /* Movimiento / ALU entera. */
    case MOp::MOV: return "mov";
    case MOp::LEA: return "lea";
    case MOp::PUSH: return "push";
    case MOp::POP: return "pop";
    case MOp::ADD: return "add";
    case MOp::SUB: return "sub";
    case MOp::IMUL: return "imul";
    case MOp::AND: return "and";
    case MOp::OR: return "or";
    case MOp::XOR: return "xor";
    case MOp::SHL: return "shl";
    case MOp::SHR: return "shr";
    case MOp::SAR: return "sar";
    case MOp::NEG: return "neg";
    case MOp::NOT: return "not";
    case MOp::IDIV: return "idiv";
    case MOp::CQO: return "cqo";
    case MOp::DIV_U: return "div";
    case MOp::MOVZX: return "movzx";
    case MOp::MOVSX: return "movsx";
    case MOp::INC: return "inc";
    case MOp::DEC: return "dec";
    case MOp::POPCNT: return "popcnt";
    case MOp::LZCNT: return "lzcnt";
    case MOp::TZCNT: return "tzcnt";
    case MOp::CMP: return "cmp";
    case MOp::TEST: return "test";
    case MOp::SETCC: return "setne";  // representante (mismos efectos: wr dst, rd flags)
    case MOp::CMOVCC: return "cmovne"; // representante (r+w dst, rd flags)
    case MOp::BSWAP: return "bswap";
    case MOp::ROL: return "rol";
    case MOp::ROR: return "ror";

    /* FP escalar f64 / f32. */
    case MOp::MOVQ_GP_XMM:
    case MOp::MOVQ_XMM_GP: return "movq";
    case MOp::SQRTSD: return "sqrtsd";
    case MOp::MINSD: return "minsd";
    case MOp::MAXSD: return "maxsd";
    case MOp::ROUNDSD: return "roundsd";
    case MOp::ADDSD: return "addsd";
    case MOp::SUBSD: return "subsd";
    case MOp::MULSD: return "mulsd";
    case MOp::DIVSD: return "divsd";
    case MOp::CVTSI2SD: return "cvtsi2sd";
    case MOp::CVTTSD2SI: return "cvttsd2si";
    case MOp::UCOMISD: return "ucomisd";
    case MOp::CVTSS2SD: return "cvtss2sd";
    case MOp::CVTSD2SS: return "cvtsd2ss";
    case MOp::MOVSD: return "movsd";
    case MOp::MOVSS: return "movss";
    case MOp::ADDSS: return "addss";
    case MOp::SUBSS: return "subss";
    case MOp::MULSS: return "mulss";
    case MOp::DIVSS: return "divss";
    case MOp::SQRTSS: return "sqrtss";
    case MOp::UCOMISS: return "ucomiss";
    case MOp::CVTSI2SS: return "cvtsi2ss";
    case MOp::CVTTSS2SI: return "cvttss2si";
    case MOp::XORPS: return "xorps";
    case MOp::ANDPS: return "andps";

    /* Packed SSE2 (f64/f32/int). */
    case MOp::ADDPD: return "addpd";
    case MOp::SUBPD: return "subpd";
    case MOp::MULPD: return "mulpd";
    case MOp::DIVPD: return "divpd";
    case MOp::MOVUPD: return "movupd";
    case MOp::MOVAPD: return "movapd";
    case MOp::SQRTPD: return "sqrtpd";
    case MOp::XORPD: return "xorpd";
    case MOp::ANDPD: return "andpd";
    case MOp::UNPCKLPD: return "unpcklpd";
    case MOp::ADDPS: return "addps";
    case MOp::SUBPS: return "subps";
    case MOp::MULPS: return "mulps";
    case MOp::DIVPS: return "divps";
    case MOp::PADDD: return "paddd";
    case MOp::PSUBD: return "psubd";
    case MOp::PADDQ: return "paddq";
    case MOp::PSUBQ: return "psubq";
    case MOp::PADDW: return "paddw";
    case MOp::PSUBW: return "psubw";
    case MOp::PMULLW: return "pmullw";
    case MOp::PADDB: return "paddb";
    case MOp::PSUBB: return "psubb";
    case MOp::PMULLD: return "pmulld";

    /* AVX 3-operandos no destructivos. */
    case MOp::VADDSD: return "vaddsd";
    case MOp::VSUBSD: return "vsubsd";
    case MOp::VMULSD: return "vmulsd";
    case MOp::VDIVSD: return "vdivsd";
    case MOp::VADDSS: return "vaddss";
    case MOp::VSUBSS: return "vsubss";
    case MOp::VMULSS: return "vmulss";
    case MOp::VDIVSS: return "vdivss";
    case MOp::VXORPS: return "vxorps";
    case MOp::VANDPS: return "vandps";
    case MOp::VFMADD231PD: return "vfmadd231pd";
    case MOp::VFMADD231PS: return "vfmadd231ps";
    case MOp::VBROADCASTSD: return "vbroadcastsd";

    default: return nullptr; // pseudo de VestaVM
    }
}

MEffects machine_effects(const MInstr &mi, EffIsa isa) {
    MEffects e;
    const Isa db_isa = (isa == EffIsa::X86) ? Isa::X86 : Isa::ARM64;
    if (const char *mnem = mop_mnemonic(mi.op, isa)) {
        if (db_effects(mi, mnem, db_isa, e)) return e;
        // Mnemonico conocido pero sin forma en la DB: barrera segura.
        e.is_barrier = true;
        e.writes_flags = e.reads_flags = e.reads_mem = e.writes_mem = true;
        add(e.reads, reg_key(mi.dst));
        add(e.reads, reg_key(mi.src1));
        add(e.reads, reg_key(mi.src2));
        return e;
    }
    pseudo_effects(mi, e);
    return e;
}

} // namespace sched
} // namespace jit
