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
 *        verdad; los registros implicitos (rax:rdx de div...) salen del regset
 * de la DB. Solo los PSEUDOS propios de VestaVM (que no existen en ninguna ISA)
 * se modelan aqui explicitamente.
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

/// Clave de registro uniforme de un operando REG/VREG (o UINT32_MAX si no lo
/// es).
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

/// Base del espacio de claves para registros que NO estan en MReg (mascaras K,
/// MMX/x87, segmento, control/debug, MSR/MXCSR, zmm16-31...): se rastrean por
/// su indice de register_set en el pool de strings -> misma cadena, misma
/// clave.
constexpr uint32_t SPECIAL_BASE = 1u << 21;

/// Sufijo numerico de un nombre de registro tras un prefijo de N letras
/// (p.ej. "XMM12" -> 12, "R8" -> 8).  -1 si no hay digitos validos.
int suffix_num(const std::string &s, size_t pref) {
    if (s.size() <= pref) return -1;
    int n = 0;
    for (size_t i = pref; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        n = n * 10 + (s[i] - '0');
    }
    return n;
}

/**
 * @brief Decodifica un @c register_set de la DB a una CLAVE de dependencia
 *        uniforme, cubriendo TODAS las clases de registro x86:
 *          - GP (A/C/D/B/SP/BP/SI/DI/R8..R15, cualquier ancho) -> id de MReg
 * 0-15 (aliasa los operandos GP explicitos).
 *          - vector XMM/YMM/ZMM 0-15 -> MReg XMM (16+n); YMMn/ZMMn aliasan
 * XMMn.
 *          - todo lo demas con NOMBRE fijo (mascaras K0-7, MMX MM0-7, x87
 * ST(n), segmento ES/CS/SS/DS/FS/GS, control CRn, debug DRn, MSR, MXCSR,
 *            ZMM16-31...) -> SPECIAL_BASE + @p regset_idx (clave estable por
 *            nombre; no esta en MReg pero se rastrea igual).
 * @return la clave, o UINT32_MAX si es una CLASE (varios regs, "GPR64", "-") o
 *         vacio -> no es un registro implicito fijo.
 */
/**
 * @brief Decodifica un register_set ARM (AArch64) a la clave de dependencia,
 *        USANDO LA MISMA NUMERACION que el target arm64 del pipeline vreg
 *        (build_arm64_target): GP x/w n -> n (0-30); FP/SIMD v/q/d/s/h/b n ->
 *        32+n (32-63).  Asi un implicito de la DB aliasa el operando explicito
 *        con el mismo registro.  SP/XZR/WZR/sistema -> especial por nombre.
 */
uint32_t regset_to_key_arm(const std::string &s, uint16_t regset_idx) {
    if (s == "SP" || s == "WSP" || s == "XZR" || s == "WZR" || s == "PC")
        return SPECIAL_BASE + regset_idx;
    const char c = s[0];
    const bool gp = (c == 'X' || c == 'W');
    const bool fp =
        (c == 'V' || c == 'Q' || c == 'D' || c == 'S' || c == 'H' || c == 'B');
    if (gp || fp) {
        const int n = suffix_num(s, 1);
        if (gp && n >= 0 && n <= 30) return static_cast<uint32_t>(n);
        if (fp && n >= 0 && n <= 31) return static_cast<uint32_t>(32 + n);
    }
    return SPECIAL_BASE + regset_idx; // NZCV/FPCR/FPSR/sistema/...
}

uint32_t regset_to_key(const char *rs, uint16_t regset_idx, EffIsa isa) {
    if (rs == nullptr || rs[0] == '\0' || rs[0] == '-') return UINT32_MAX;
    for (const char *p = rs; *p; ++p)
        if (*p == '/') return UINT32_MAX; // clase (lista de regs)
    const std::string s(rs);

    if (isa != EffIsa::X86) return regset_to_key_arm(s, regset_idx);

    // --- GP (familia de una letra o Rn, cualquier ancho) ---
    struct Fam {
        const char *k;
        int reg;
    };
    static const Fam fam[] = {
        {"AX", 0},  {"EAX", 0}, {"RAX", 0}, {"AL", 0},  {"AH", 0},  {"CX", 1},
        {"ECX", 1}, {"RCX", 1}, {"CL", 1},  {"CH", 1},  {"DX", 2},  {"EDX", 2},
        {"RDX", 2}, {"DL", 2},  {"DH", 2},  {"BX", 3},  {"EBX", 3}, {"RBX", 3},
        {"BL", 3},  {"BH", 3},  {"SP", 4},  {"ESP", 4}, {"RSP", 4}, {"SPL", 4},
        {"BP", 5},  {"EBP", 5}, {"RBP", 5}, {"BPL", 5}, {"SI", 6},  {"ESI", 6},
        {"RSI", 6}, {"SIL", 6}, {"DI", 7},  {"EDI", 7}, {"RDI", 7}, {"DIL", 7},
    };
    for (const Fam &f : fam)
        if (s == f.k) return static_cast<uint32_t>(f.reg);
    if ((s[0] == 'R') && s.size() >= 2 && s[1] >= '8' && s[1] <= '9') {
        // R8..R15 con sufijos D/W/B opcionales.
        int n = s[1] - '0';
        if (s.size() >= 3 && s[2] >= '0' && s[2] <= '5') n = 10 + (s[2] - '0');
        if (n >= 8 && n <= 15) return static_cast<uint32_t>(n);
    }

    // --- vector XMM/YMM/ZMM (SSE/AVX/AVX512): 0-15 aliasan MReg XMM ---
    if (s.size() >= 4 &&
        (s.rfind("XMM", 0) == 0 || s.rfind("YMM", 0) == 0 ||
         s.rfind("ZMM", 0) == 0)) {
        const int n = suffix_num(s, 3);
        if (n >= 0 && n <= 15)
            return static_cast<uint32_t>(static_cast<int>(MReg::XMM0) + n);
        // ZMM16..31 (AVX512): no estan en MReg -> clave especial por nombre.
    }

    // --- resto de clases con nombre fijo (K/MM/ST/seg/CR/DR/MSR/MXCSR/...) ---
    // No estan en MReg; se rastrean por su indice de register_set (estable).
    return SPECIAL_BASE + regset_idx;
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
    default: return false; // NONE
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
 * @brief Rol de los operandos de un MOp REAL.  Es la DEFINICION del MOp (que
 *        hace la operacion), NO un dato de ISA a extraer -> inequivoco.  Los
 *        registros IMPLICITOS con nombre (rax:rdx de div) SI son ISA y salen de
 *        la DB (@c div_family), no de aqui.
 */
struct OpRoles {
    bool dst_written = false; ///< el slot dst se ESCRIBE
    bool dst_read = false;    ///< el slot dst se LEE (two-address / compare)
    bool writes_flags = false;
    bool reads_flags = false;
    bool div_family = false; ///< consultar la DB por el implicito rax:rdx
};

/// Clasifica los roles de un MOp real (los mismos que enumera @ref
/// mop_mnemonic).
OpRoles mop_roles(MOp op) {
    OpRoles r;
    switch (op) {
    /* Movimientos / cargas / conversiones / direcciones: dst SOLO escrito. */
    case MOp::MOV:
    case MOp::LEA:
    case MOp::MOVZX:
    case MOp::MOVSX:
    case MOp::MOVQ_GP_XMM:
    case MOp::MOVQ_XMM_GP:
    case MOp::CVTSI2SD:
    case MOp::CVTTSD2SI:
    case MOp::CVTSS2SD:
    case MOp::CVTSD2SS:
    case MOp::CVTSI2SS:
    case MOp::CVTTSS2SI:
    case MOp::MOVSD:
    case MOp::MOVSS:
    case MOp::MOVUPD:
    case MOp::MOVAPD: r.dst_written = true; break;
    /* SETcc: escribe dst, LEE flags. */
    case MOp::SETCC:
        r.dst_written = true;
        r.reads_flags = true;
        break;
    /* CMOVcc: dst read+written (condicional), LEE flags. */
    case MOp::CMOVCC:
        r.dst_written = true;
        r.dst_read = true;
        r.reads_flags = true;
        break;

    /* ALU entera 2-address: dst read+written + escribe flags. */
    case MOp::ADD:
    case MOp::SUB:
    case MOp::IMUL:
    case MOp::AND:
    case MOp::OR:
    case MOp::XOR:
    case MOp::SHL:
    case MOp::SHR:
    case MOp::SAR:
    case MOp::NEG:
    case MOp::INC:
    case MOp::DEC:
    case MOp::ROL:
    case MOp::ROR:
        r.dst_written = true;
        r.dst_read = true;
        r.writes_flags = true;
        break;
    /* NOT / BSWAP: dst read+written, SIN flags. */
    case MOp::NOT:
    case MOp::BSWAP:
        r.dst_written = true;
        r.dst_read = true;
        break;
    /* POPCNT/LZCNT/TZCNT: dst SOLO escrito, escribe flags. */
    case MOp::POPCNT:
    case MOp::LZCNT:
    case MOp::TZCNT:
        r.dst_written = true;
        r.writes_flags = true;
        break;

    /* CMP / TEST / UCOMIS*: leen ambos (dst LEIDO), escriben flags, sin
     * destino. */
    case MOp::CMP:
    case MOp::TEST:
    case MOp::UCOMISD:
    case MOp::UCOMISS:
        r.dst_read = true;
        r.writes_flags = true;
        break;

    /* FP/SIMD 2-address (arith): dst read+written, sin flags de enteros. */
    case MOp::ADDSD:
    case MOp::SUBSD:
    case MOp::MULSD:
    case MOp::DIVSD:
    case MOp::MINSD:
    case MOp::MAXSD:
    case MOp::SQRTSD:
    case MOp::ROUNDSD:
    case MOp::ADDSS:
    case MOp::SUBSS:
    case MOp::MULSS:
    case MOp::DIVSS:
    case MOp::SQRTSS:
    case MOp::XORPS:
    case MOp::ANDPS:
    case MOp::ADDPD:
    case MOp::SUBPD:
    case MOp::MULPD:
    case MOp::DIVPD:
    case MOp::SQRTPD:
    case MOp::XORPD:
    case MOp::ANDPD:
    case MOp::UNPCKLPD:
    case MOp::SHUFPS: // dst,src,imm -> lanes 0,1 vienen de dst (read+written)
    case MOp::ADDPS:
    case MOp::SUBPS:
    case MOp::MULPS:
    case MOp::DIVPS:
    case MOp::PADDD:
    case MOp::PSUBD:
    case MOp::PADDQ:
    case MOp::PSUBQ:
    case MOp::PADDW:
    case MOp::PSUBW:
    case MOp::PMULLW:
    case MOp::PADDB:
    case MOp::PSUBB:
    case MOp::PMULLD:
    case MOp::VFMADD231PD: // FMA: dst es acumulador (read+written)
    case MOp::VFMADD231PS:
    case MOp::VFMSUB231PD: // FMSUB: dst = a*b - dst (tambien read+written)
    case MOp::VFMSUB231PS:
        r.dst_written = true;
        r.dst_read = true;
        break;
    /* AVX 3-operandos NO destructivo: dst SOLO escrito. */
    case MOp::VADDSD:
    case MOp::VSUBSD:
    case MOp::VMULSD:
    case MOp::VDIVSD:
    case MOp::VADDSS:
    case MOp::VSUBSS:
    case MOp::VMULSS:
    case MOp::VDIVSS:
    case MOp::VXORPS:
    case MOp::VANDPS:
    case MOp::VBROADCASTSD:
    case MOp::VBROADCASTSS: r.dst_written = true; break;

    /* Division entera: el operando explicito es el divisor (LEIDO); el
     * resultado va a rax:rdx (implicito de la DB); escribe flags. */
    case MOp::IDIV:
    case MOp::DIV_U:
        r.dst_read = true;
        r.writes_flags = true;
        r.div_family = true;
        break;
    case MOp::CQO:
        r.div_family = true; // rax -> rdx, implicito de la DB
        break;

    default:
        // Todo MOp real esta arriba; si aparece uno nuevo sin clasificar, se
        // trata como read+write de dst + flags (seguro, no optimista de menos).
        r.dst_written = true;
        r.dst_read = true;
        r.writes_flags = true;
        break;
    }
    return r;
}

/**
 * @brief Anade los registros IMPLICITOS con nombre (rax:rdx) de una instruccion
 *        de la familia div, leidos de la DB (regset).  El mnemonico de div
 *        (idiv/div/cqo) NO es ambiguo -> el match es fiable.
 */
void add_div_implicit_from_db(const MInstr &mi, const char *mnem, Isa isa,
                              MEffects &e) {
    std::vector<ParsedOp> ops;
    const int nexp = explicit_operand_count(mi);
    for (int s = 0; s < nexp; ++s) {
        ParsedOp p;
        if (to_parsed(minstr_slot(mi, s), p)) ops.push_back(p);
    }
    const int32_t fid = vx::instr_db::match(isa, mnem, ops);
    if (fid < 0) return;
    const vx::instr_db::IsaData &db =
        (isa == Isa::X86) ? vx::instr_db::db_x86() : vx::instr_db::db_arm64();
    if (fid >= static_cast<int32_t>(db.form_count)) return;
    const DbForm &f = db.forms[static_cast<uint32_t>(fid)];
    for (uint8_t i = 0; i < f.ops_count; ++i) {
        const DbOperand &o = db.ops[f.ops_off + i];
        if ((o.flags & 0x4) == 0) continue;           // solo implicitos
        if (o.kind != vx::instr_db::OP_REG) continue; // solo registros
        if (o.regset >= db.str_count) continue;
        const uint32_t k = regset_to_key(db.str[o.regset], o.regset, isa);
        if (k == UINT32_MAX) continue;
        if (o.flags & 0x1) add(e.reads, k);
        if (o.flags & 0x2) add(e.writes, k);
    }
}

/**
 * @brief Efectos de una instruccion REAL: roles explicitos (definicion del MOp)
 *        + flags + los implicitos con nombre de la DB (familia div).
 */
void real_effects(const MInstr &mi, const char *mnem, Isa isa, MEffects &e) {
    // CQO/CDQ: sign-extienden RAX en RDX:RAX (semantica FIJA, sin operandos):
    // LEEN RAX y ESCRIBEN RDX, SIN tocar flags.  Es IMPRESCINDIBLE modelar el
    // RDX escrito -- si no, un `mov rdx, imm` puede colarse entre el CQO y el
    // IDIV/DIV que lee RDX:RAX como dividendo, corrompiendo la division (#DE o
    // resultado erroneo).  La familia div de la DB no siempre expone este RDX
    // implicito para CQO, asi que se modela aqui explicito (semantica exacta).
    if (mi.op == MOp::CQO) {
        add(e.reads, static_cast<uint8_t>(MReg::RAX));
        add(e.writes, static_cast<uint8_t>(MReg::RDX));
        return;
    }
    const OpRoles r = mop_roles(mi.op);
    e.writes_flags = r.writes_flags;
    e.reads_flags = r.reads_flags;

    // dst: escrito y/o leido segun el rol; si es MEM, define store/load.
    if (mi.dst.kind == MOperandKind::MEM) {
        add_mem_addr_reads(e, mi.dst);
        if (r.dst_written) e.writes_mem = true;
        if (r.dst_read) e.reads_mem = true;
    } else if (mi.dst.kind != MOperandKind::NONE) {
        if (r.dst_written) add(e.writes, reg_key(mi.dst));
        if (r.dst_read) add(e.reads, reg_key(mi.dst));
    }
    // src1/src2: siempre LEIDOS; MEM -> lee memoria + regs de direccion.
    for (const MOperand *s : {&mi.src1, &mi.src2}) {
        if (s->kind == MOperandKind::MEM) {
            add_mem_addr_reads(e, *s);
            e.reads_mem = true;
        } else if (s->kind != MOperandKind::NONE) {
            add(e.reads, reg_key(*s));
        }
    }
    // Familia div (IDIV/DIV): el dividendo es RDX:RAX -- LEE ambos -- y deja el
    // cociente en RAX y el resto en RDX -- ESCRIBE ambos.  Es semantica FIJA de
    // x86 (no depende de la microarq), asi que se modela EXPLICITO: la
    // extraccion de implicitos de la DB no siempre expone el RDX LEIDO, y sin
    // el, un `mov rdx, imm` puede colarse ANTES del div (WAR idiv/mov-rdx
    // perdida) y corromper el dividendo -> #DE o resultado erroneo.
    if (r.div_family) {
        if (isa == Isa::X86) {
            add(e.reads, static_cast<uint8_t>(MReg::RAX));
            add(e.reads, static_cast<uint8_t>(MReg::RDX));
            add(e.writes, static_cast<uint8_t>(MReg::RAX));
            add(e.writes, static_cast<uint8_t>(MReg::RDX));
        } else {
            add_div_implicit_from_db(mi, mnem, isa, e);
        }
    }
}

/**
 * @brief Modela los efectos de un PSEUDO de VestaVM (no existe en la ISA).
 *        Cada pseudo tiene una semantica fija y conocida por construccion.
 */
void pseudo_effects(const MInstr &mi, MEffects &e) {
    switch (mi.op) {
    /* Sin efecto de datos y movibles libremente. */
    case MOp::NOP:
    case MOp::COMMENT: break;

    /* Posiciones FIJAS: un LABEL_DEF es destino de salto y las entradas de
     * jump-table son datos inline referenciados por su offset -> barrera (nada
     * se reordena a traves de ellas). */
    case MOp::LABEL_DEF:
    case MOp::DATA_PTR_LABEL:
    case MOp::DATA_REL32_LABEL: e.is_barrier = true; break;

    /* ARG: marca un argumento -> LEE su src1 (para no adelantar al productor).
     */
    case MOp::ARG: add(e.reads, reg_key(mi.src1)); break;

    /* PUSH src: lee src + rsp, escribe rsp + memoria. */
    case MOp::PUSH:
        add(e.reads, reg_key(mi.dst));  // el operando de PUSH viaja en dst
        add(e.reads, reg_key(mi.src1)); // (o src1 segun el selector)
        add(e.reads, static_cast<uint8_t>(MReg::RSP));
        add(e.writes, static_cast<uint8_t>(MReg::RSP));
        e.writes_mem = true;
        break;
    /* POP dst: escribe dst + rsp, lee rsp + memoria. */
    case MOp::POP:
        add(e.writes, reg_key(mi.dst));
        add(e.reads, static_cast<uint8_t>(MReg::RSP));
        add(e.writes, static_cast<uint8_t>(MReg::RSP));
        e.reads_mem = true;
        break;

    /* Cargas pseudo: dst = [addr]. */
    case MOp::LOAD:    // dst, src1=addr
    case MOp::LOAD_VM: // dst, src1=addr, src2=imm64_idx (fallback)
        add(e.writes, reg_key(mi.dst));
        add(e.reads, reg_key(mi.src1));
        e.reads_mem = true;
        break;
    /* Stores pseudo: [addr] = val. */
    case MOp::STORE:    // src1=addr, src2=val
    case MOp::STORE_VM: // src1=addr, src2=val, dst=imm64_idx
        add(e.reads, reg_key(mi.src1));
        add(e.reads, reg_key(mi.src2));
        e.writes_mem = true;
        break;

    /* ALLOCA: dst = puntero a espacio reservado del frame. */
    case MOp::ALLOCA:
    case MOp::ALLOCA_VM: add(e.writes, reg_key(mi.dst)); break;

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
    case MOp::ATOMICCAS_V: // dst in/out (expected->old), src1=addr,
                           // src2=desired
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
    case MOp::TLS_PE_ADDR: add(e.writes, reg_key(mi.dst)); break;

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
    case MOp::JMP: e.is_barrier = true; break;
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
    case MOp::INLINE_ASM_RAW: e.is_barrier = true; break;

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
    // arm64: los MOp enteros genericos se comparten; los A64_* son propios. Los
    // saltos/llamadas (b/bl/ret/cbz) NO se mapean -> pseudo (barrera), igual
    // que en x86.  Se usa para los efectos DB (machine_effects) y el coste DB.
    if (isa == EffIsa::ARM64) {
        switch (op) {
        case MOp::MOV: return "mov";
        case MOp::ADD: return "add";
        case MOp::SUB: return "sub";
        case MOp::AND: return "and";
        case MOp::OR: return "orr";
        case MOp::XOR: return "eor";
        case MOp::SHL: return "lsl";
        case MOp::SHR: return "lsr";
        case MOp::SAR: return "asr";
        case MOp::NEG: return "neg";
        case MOp::NOT: return "mvn";
        case MOp::CMP: return "cmp";
        case MOp::LOAD: return "ldr";
        case MOp::STORE: return "str";
        case MOp::A64_UDIV: return "udiv";
        case MOp::A64_SDIV: return "sdiv";
        case MOp::A64_MADD: return "madd";
        case MOp::A64_MSUB: return "msub";
        case MOp::A64_CSEL: return "csel";
        case MOp::A64_CSET: return "cset";
        case MOp::A64_MVN: return "mvn";
        case MOp::A64_MOVZ: return "movz";
        case MOp::A64_MOVK: return "movk";
        case MOp::A64_SXTB: return "sxtb";
        case MOp::A64_UXTB: return "uxtb";
        case MOp::A64_FADD: return "fadd";
        case MOp::A64_FSUB: return "fsub";
        case MOp::A64_FMUL: return "fmul";
        case MOp::A64_FDIV: return "fdiv";
        case MOp::A64_FCMP: return "fcmp";
        case MOp::A64_FMOV: return "fmov";
        case MOp::A64_FNEG: return "fneg";
        case MOp::A64_FABS: return "fabs";
        case MOp::A64_FSQRT: return "fsqrt";
        case MOp::A64_SCVTF: return "scvtf";
        case MOp::A64_UCVTF: return "ucvtf";
        case MOp::A64_FCVTZS: return "fcvtzs";
        case MOp::A64_FCVTZU: return "fcvtzu";
        case MOp::A64_FCVT: return "fcvt";
        default: return nullptr; // pseudo / salto / no mapeado aun
        }
    }
    if (isa != EffIsa::X86) return nullptr; // arm32/riscv: sin mapeo aun

    switch (op) {
    /* Movimiento / ALU entera. */
    case MOp::MOV: return "mov";
    case MOp::LEA: return "lea";
    /* PUSH/POP: NO reales aqui -> pseudo (tocan rsp + memoria implicitos). */
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
    case MOp::SETCC:
        return "setne"; // representante (mismos efectos: wr dst, rd flags)
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
    case MOp::VFMSUB231PD: return "vfmsub231pd";
    case MOp::VFMSUB231PS: return "vfmsub231ps";
    case MOp::VBROADCASTSD: return "vbroadcastsd";
    case MOp::VBROADCASTSS: return "vbroadcastss";
    case MOp::SHUFPS: return "shufps";

    default: return nullptr; // pseudo de VestaVM
    }
}

MEffects machine_effects(const MInstr &mi, EffIsa isa) {
    MEffects e;
    const Isa db_isa = (isa == EffIsa::X86) ? Isa::X86 : Isa::ARM64;
    if (const char *mnem = mop_mnemonic(mi.op, isa)) {
        real_effects(mi, mnem, db_isa, e);
        return e;
    }
    pseudo_effects(mi, e);
    return e;
}

} // namespace sched
} // namespace jit
