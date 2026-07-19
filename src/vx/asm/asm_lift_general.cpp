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
 * @file vx/asm/asm_lift_general.cpp
 * @brief Lift general instruccion-a-instruccion de asm a IR SSA (subset entero
 *        straight-line).  Ver asm_lift_general.h.
 */

#include "vx/asm/asm_lift_general.h"

#include "ir/ssa_ir.h"
#include "vx/asm/asm_effects.h"   // asm_canonical_reg
#include "vx/asm/asm_phys_reg.h"  // asm_x86_gp_index (ancho del operando)

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace vx {

namespace {

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

/// Trocea una linea de asm en mnemonico + operandos (por comas).
void split_insn(const std::string &line, std::string &mnem,
                std::vector<std::string> &ops) {
    mnem.clear();
    ops.clear();
    const std::string s = trim(line);
    size_t i = 0;
    while (i < s.size() && !std::isspace((unsigned char)s[i])) ++i;
    mnem = s.substr(0, i);
    for (char &c : mnem) c = (char)std::tolower((unsigned char)c);
    std::string rest = trim(s.substr(i));
    if (rest.empty()) return;
    size_t start = 0;
    for (size_t j = 0; j <= rest.size(); ++j)
        if (j == rest.size() || rest[j] == ',') {
            ops.push_back(trim(rest.substr(start, j - start)));
            start = j + 1;
        }
}

/// Divide el cuerpo en instrucciones, descartando comentarios (// ;) y labels.
std::vector<std::string> instructions(const std::string &body) {
    std::vector<std::string> out;
    std::string line;
    for (size_t i = 0; i <= body.size(); ++i) {
        if (i == body.size() || body[i] == '\n') {
            std::string l = line;
            line.clear();
            // Quitar comentario.
            size_t c = l.find("//");
            if (c != std::string::npos) l = l.substr(0, c);
            c = l.find(';');
            if (c != std::string::npos) l = l.substr(0, c);
            l = trim(l);
            if (l.empty()) continue;
            if (l.back() == ':') continue; // label
            out.push_back(l);
        } else {
            line += body[i];
        }
    }
    return out;
}

bool parse_imm(const std::string &op, int64_t &out); // fwd

/// Un termino del modo de direccionamiento x86 @c [base + index*scale + disp].
struct MemAddr {
    std::string base;  ///< registro base canonico ("" si no hay)
    std::string index; ///< registro index canonico ("" si no hay)
    int64_t scale = 1; ///< 1/2/4/8
    int64_t disp = 0;  ///< desplazamiento constante
    bool ok = false;   ///< true si @p op es una memoria [ ... ] valida
};

/// Parsea CUALQUIER modo de direccionamiento x86 @c [base + index*scale + disp]
/// (base, index*scale, disp opcionales; el prefijo @c "qword ptr" se ignora).
MemAddr parse_mem(const std::string &opr) {
    MemAddr a;
    std::string op = trim(opr);
    // Ignorar el size-hint tipo "qword ptr [..]".
    const size_t br = op.find('[');
    if (br == std::string::npos || op.back() != ']') return a;
    std::string in = trim(op.substr(br + 1, op.size() - br - 2));
    // Trocear por + / - conservando el signo del termino.
    std::vector<std::pair<int, std::string>> terms; // (signo, token)
    int sign = 1;
    std::string tok;
    auto flush = [&]() {
        std::string t = trim(tok);
        if (!t.empty()) terms.push_back({sign, t});
        tok.clear();
    };
    for (char c : in) {
        if (c == '+') { flush(); sign = 1; }
        else if (c == '-') { flush(); sign = -1; }
        else tok += c;
    }
    flush();
    for (auto &pr : terms) {
        const std::string &t = pr.second;
        const size_t star = t.find('*');
        if (star != std::string::npos) { // index*scale
            const std::string ir = asm_canonical_reg(trim(t.substr(0, star)));
            int64_t sc = 0;
            if (ir.empty() || !parse_imm(trim(t.substr(star + 1)), sc)) return a;
            a.index = ir;
            a.scale = sc;
            continue;
        }
        int64_t d = 0;
        if (parse_imm(t, d)) { a.disp += pr.first * d; continue; }
        const std::string r = asm_canonical_reg(t);
        if (r.empty()) return a;
        if (a.base.empty()) a.base = r;
        else if (a.index.empty()) { a.index = r; a.scale = 1; }
        else return a; // >2 registros: no soportado
    }
    a.ok = true;
    return a;
}

/// ¿Es @p op un operando de memoria @c [ ... ]?
bool is_mem(const std::string &op) {
    return op.find('[') != std::string::npos && trim(op).back() == ']';
}

/// Memoria @c [reg] PLANA (sin index/disp) -> registro canonico; "" si no lo es.
/// Reusa @ref parse_mem (el modo de direccionamiento completo) restringiendo al
/// caso base puro.
std::string plain_mem(const std::string &op) {
    MemAddr a = parse_mem(op);
    if (!a.ok || a.base.empty() || !a.index.empty() || a.disp != 0) return "";
    return a.base;
}

/// Intenta parsear un inmediato entero (dec/hex/negativo).  Los positivos usan
/// @c strtoull para cubrir el rango COMPLETO de 64 bits sin signo (p.ej. el
/// idioma @c mov @c rax,0xFFFFFFFFFFFFFFFF); @c strtoll lo saturaria a
/// @c INT64_MAX y produciria un valor equivocado.
bool parse_imm(const std::string &op, int64_t &out) {
    if (op.empty()) return false;
    char c0 = op[0];
    if (!(std::isdigit((unsigned char)c0) || c0 == '-' || c0 == '+')) return false;
    char *end = nullptr;
    if (c0 == '-') {
        const long long v = std::strtoll(op.c_str(), &end, 0);
        if (end == op.c_str() || (end && *end != '\0')) return false;
        out = (int64_t)v;
    } else {
        const unsigned long long v = std::strtoull(op.c_str(), &end, 0);
        if (end == op.c_str() || (end && *end != '\0')) return false;
        out = (int64_t)v; // reinterpretacion de bits (los backends usan u64)
    }
    return true;
}

/// Ancho en bits de un size-hint de memoria (@c byte/word/dword/qword @c ptr).
/// 0 si no hay hint explicito (el ancho lo aporta el registro del otro
/// operando).
int mem_hint_width(const std::string &op) {
    const size_t br = op.find('[');
    if (br == std::string::npos) return 0;
    std::string pre;
    for (char c : op.substr(0, br))
        pre.push_back((char)std::tolower((unsigned char)c));
    pre = trim(pre);
    const std::string hint = pre.substr(0, pre.find(' '));
    if (hint == "byte") return 8;
    if (hint == "word") return 16;
    if (hint == "dword") return 32;
    if (hint == "qword") return 64;
    return 0;
}

/// Mapea el sufijo de un @c setCC (lo que sigue a "set") al IrOp de comparacion
/// y su signedness (para leer los operandos del cmp con signo/sin signo).  Para
/// eq/ne el signo es irrelevante (se usa sin signo).  @c false si no se conoce.
bool setcc_to_cmp(const std::string &cc, ir::IrOp &op, bool &sgn) {
    sgn = false;
    if (cc == "e" || cc == "z") { op = ir::IrOp::CMP_EQ; return true; }
    if (cc == "ne" || cc == "nz") { op = ir::IrOp::CMP_NE; return true; }
    sgn = true;
    if (cc == "l" || cc == "nge") { op = ir::IrOp::CMP_LT; return true; }
    if (cc == "g" || cc == "nle") { op = ir::IrOp::CMP_GT; return true; }
    if (cc == "le" || cc == "ng") { op = ir::IrOp::CMP_LE; return true; }
    if (cc == "ge" || cc == "nl") { op = ir::IrOp::CMP_GE; return true; }
    sgn = false;
    if (cc == "b" || cc == "c" || cc == "nae") { op = ir::IrOp::CMP_ULT; return true; }
    if (cc == "a" || cc == "nbe") { op = ir::IrOp::CMP_UGT; return true; }
    if (cc == "be" || cc == "na") { op = ir::IrOp::CMP_ULE; return true; }
    if (cc == "ae" || cc == "nb" || cc == "nc") { op = ir::IrOp::CMP_UGE; return true; }
    return false; // cc no soportado (o/no/s/ns/p/np...) -> el par no encaja
}

/// IrOp binario (2-address: dst = dst OP src) para un mnemonico ALU x86.
bool binop_of(const std::string &m, ir::IrOp &op) {
    if (m == "add") { op = ir::IrOp::ADD; return true; }
    if (m == "sub") { op = ir::IrOp::SUB; return true; }
    if (m == "imul") { op = ir::IrOp::MUL; return true; }
    if (m == "and") { op = ir::IrOp::AND; return true; }
    if (m == "or") { op = ir::IrOp::OR; return true; }
    if (m == "xor") { op = ir::IrOp::XOR; return true; }
    if (m == "shl" || m == "sal") { op = ir::IrOp::SHL; return true; }
    if (m == "shr") { op = ir::IrOp::SHR; return true; }
    if (m == "sar") { op = ir::IrOp::SAR; return true; }
    return false;
}

/// Emisores IR minimos.
ir::IrValueId emit_const(ir::IrFunction &fn, uint32_t blk, int64_t v,
                         uint32_t line) {
    const ir::IrValueId d = fn.new_value(ir::IrType::I64);
    ir::IrInstr in{};
    in.op = ir::IrOp::CONST;
    in.type = ir::IrType::I64;
    in.dst = d;
    in.imm = (uint64_t)v;
    in.source_line = line;
    fn.append(blk, std::move(in));
    return d;
}
ir::IrValueId emit_bin(ir::IrFunction &fn, uint32_t blk, ir::IrOp op,
                       ir::IrValueId a, ir::IrValueId b, uint32_t line) {
    const ir::IrValueId d = fn.new_value(ir::IrType::I64);
    ir::IrInstr in{};
    in.op = op;
    in.type = ir::IrType::I64;
    in.dst = d;
    in.operands = {a, b};
    in.source_line = line;
    fn.append(blk, std::move(in));
    return d;
}
ir::IrValueId emit_un(ir::IrFunction &fn, uint32_t blk, ir::IrOp op,
                      ir::IrValueId a, uint32_t line) {
    const ir::IrValueId d = fn.new_value(ir::IrType::I64);
    ir::IrInstr in{};
    in.op = op;
    in.type = ir::IrType::I64;
    in.dst = d;
    in.operands = {a};
    in.source_line = line;
    fn.append(blk, std::move(in));
    return d;
}
/// Tipo IR para un acceso a memoria de @p w bits.  Los LOAD usan el tipo SIN
/// signo (zero-extend, la semantica de un @c mov de x86 a un registro parcial);
/// los STORE solo miran el ANCHO.
ir::IrType mem_ty(int w, bool is_load) {
    switch (w) {
    case 8: return is_load ? ir::IrType::U8 : ir::IrType::I8;
    case 16: return is_load ? ir::IrType::U16 : ir::IrType::I16;
    case 32: return is_load ? ir::IrType::U32 : ir::IrType::I32;
    default: return ir::IrType::I64;
    }
}
/// LOAD de @p w bits desde @p addr, zero-extendido a I64.  @p host: si la
/// direccion apunta a memoria HOST (el IR emite @c movh/loadzh); un operando de
/// memoria de un asm inline es SIEMPRE host, un slot de variable (ALLOCA) es VM.
ir::IrValueId emit_load(ir::IrFunction &fn, uint32_t blk, ir::IrValueId addr,
                        int w, bool host, uint32_t line) {
    if (host) fn.values[addr].is_host_ptr = true; // el emitter mira el operando
    const ir::IrValueId d = fn.new_value(ir::IrType::I64);
    ir::IrInstr in{};
    in.op = ir::IrOp::LOAD;
    in.type = mem_ty(w, /*is_load=*/true);
    in.dst = d;
    in.operands = {addr};
    in.source_line = line;
    fn.append(blk, std::move(in));
    fn.values[d].is_host_ptr = false; // el valor cargado es un entero, no un ptr
    return d;
}
/// STORE de los @p w bits bajos de @p val en @p addr.  @p host: ver @c emit_load.
void emit_store(ir::IrFunction &fn, uint32_t blk, ir::IrValueId val,
                ir::IrValueId addr, int w, bool host, uint32_t line) {
    if (host) fn.values[addr].is_host_ptr = true;
    ir::IrInstr in{};
    in.op = ir::IrOp::STORE;
    in.type = mem_ty(w, /*is_load=*/false);
    in.operands = {val, addr};
    in.source_line = line;
    fn.append(blk, std::move(in));
}

/// Mascara de @p w bits bajos como u64 (w>=64 -> todos los bits).
uint64_t width_mask(int w) {
    return w >= 64 ? ~0ull : ((1ull << w) - 1ull);
}

/// (canon, ancho, byte-alto).  Devuelve el ancho en bits (8/16/32/64) del
/// registro GP @p tok, 0 si no es un GP entero nombrable (vector/desconocido).
int reg_info(const std::string &tok, std::string &canon, bool &is_high) {
    is_high = false;
    std::string s;
    s.reserve(tok.size());
    for (char c : tok) s.push_back((char)std::tolower((unsigned char)c));
    // trim
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    s = s.substr(a, b - a);
    canon = asm_canonical_reg(s);
    if (canon.empty()) return 0;      // no es registro (o arch no x86)
    if (canon[0] == 'v') return 0;    // vector (xmm/ymm/zmm) -> no GP entero
    if (s == "ah" || s == "bh" || s == "ch" || s == "dh") {
        is_high = true;
        return 8;
    }
    uint16_t w = 0;
    const int idx = vx::asm_x86_gp_index(s, &w);
    if (idx >= 0) return (int)w;
    return 0; // canon no vacio pero ancho desconocido -> conservador
}

/* ===================================================================== */
/* Lifter por ISA.  El CORE de emision (emit_*, width_mask, mem_ty) y el   */
/* enhebrado SSA son NEUTROS: producen IrOps genericos (ADD, LOAD,         */
/* POPCNT...).  Lo que es POR-ISA -- reconocimiento de mnemonicos, nombres */
/* y anchos de registro, modos de direccionamiento, semantica de registro */
/* parcial (x86 zero-extend a 32 / preserva 8-16; arm64 zero-extend a w) --*/
/* vive en el lifter de cada arquitectura.  Anadir un ISA = anadir su      */
/* lift_<isa> reusando el core; el IR resultante es el mismo para todas.   */
/* ===================================================================== */

/** @brief Lifter x86/x86-64: reconoce el subset entero straight-line y lo baja
 *  a IR neutro.  reg_info/binop_of/parse_mem/mem_hint_width son la parte x86. */
bool lift_x86(
    ir::IrFunction &fn, uint32_t block, const std::string &body,
    const std::unordered_map<std::string, AsmBoundReg> &bound, uint32_t line) {
    const std::vector<std::string> insns = instructions(body);
    if (insns.empty()) return false;

    // Estado SSA por registro: cur[canon] = valor ARQUITECToNICO COMPLETO de
    // 64 bits del registro fisico (rax), con los bits altos correctos segun las
    // reglas de x86.  wrote = registros LIGADOS escritos (para el write-back).
    std::unordered_map<std::string, ir::IrValueId> cur;
    std::vector<std::string> wrote;
    auto mark_write = [&](const std::string &r) {
        if (bound.find(r) == bound.end()) return; // temporal -> sin write-back
        for (const std::string &w : wrote)
            if (w == r) return;
        wrote.push_back(r);
    };

    // Emisores cortos.
    auto K = [&](int64_t v) { return emit_const(fn, block, v, line); };
    auto BIN = [&](ir::IrOp op, ir::IrValueId a, ir::IrValueId b) {
        return emit_bin(fn, block, op, a, b, line);
    };
    // Los @p w bits bajos de @p v, zero-extendidos (AND con la mascara).
    auto and_mask = [&](ir::IrValueId v, int w) -> ir::IrValueId {
        if (w >= 64) return v;
        return BIN(ir::IrOp::AND, v, K((int64_t)width_mask(w)));
    };
    // Los @p w bits bajos de @p v, sign-extendidos a 64 (SHL 64-w; SAR 64-w).
    auto sext_low = [&](ir::IrValueId v, int w) -> ir::IrValueId {
        if (w >= 64) return v;
        return BIN(ir::IrOp::SAR, BIN(ir::IrOp::SHL, v, K(64 - w)), K(64 - w));
    };

    // Valor completo de 64 bits de un registro: si esta en cur, ese; si esta
    // ligado, lo carga de su slot al ancho del var (zero-extendido); si no,
    // falla (registro no-ligado leido antes de escribirse).
    auto get_full = [&](const std::string &canon, bool &ok) -> ir::IrValueId {
        ok = true;
        auto it = cur.find(canon);
        if (it != cur.end()) return it->second;
        auto b = bound.find(canon);
        if (b == bound.end()) { ok = false; return 0; }
        // El slot del var register-bound conserva su naturaleza (host/VM) en el
        // flag del valor del ALLOCA: lo respetamos (los register() suelen vivir
        // en memoria host -- alloc + movh).
        const bool slot_host = fn.values[b->second.slot].is_host_ptr;
        const ir::IrValueId v = emit_load(fn, block, b->second.slot,
                                          b->second.width_bits, slot_host, line);
        cur[canon] = v;
        return v;
    };
    // Lee un registro al ancho @p w (byte-alto @p is_high), con signo si @p sgn.
    auto read_reg = [&](const std::string &canon, int w, bool is_high, bool sgn,
                        bool &ok) -> ir::IrValueId {
        ir::IrValueId full = get_full(canon, ok);
        if (!ok) return 0;
        ir::IrValueId v = is_high ? BIN(ir::IrOp::SHR, full, K(8)) : full;
        if (w >= 64) return v;
        return sgn ? sext_low(v, w) : and_mask(v, w);
    };
    // Lee un OPERANDO (registro o inmediato) al ancho @p w.
    auto read_op = [&](const std::string &op, int w, bool sgn,
                       bool &ok) -> ir::IrValueId {
        int64_t imm = 0;
        if (parse_imm(op, imm)) { ok = true; return K(imm); }
        std::string canon;
        bool is_high = false;
        const int rw = reg_info(op, canon, is_high);
        if (rw == 0) { ok = false; return 0; }
        return read_reg(canon, w, is_high, sgn, ok);
    };
    // Escribe @p r en @p canon al ancho @p w con la semantica x86: 64 pisa todo;
    // 32 pone a cero los altos; 8/16 (y byte-alto) preservan los altos ->
    // necesitan el valor previo.  Devuelve false si un parcial 8/16 no tiene
    // valor previo (registro no-ligado sin escribir) -> el bloque no es liftable.
    auto write_reg = [&](const std::string &canon, int w, bool is_high,
                         ir::IrValueId r, bool &ok) {
        ok = true;
        if (w >= 64) { cur[canon] = r; mark_write(canon); return; }
        if (w == 32) { cur[canon] = and_mask(r, 32); mark_write(canon); return; }
        // 8/16: combinar con los bits altos previos.
        bool have = true;
        const ir::IrValueId old = get_full(canon, have);
        if (!have) { ok = false; return; } // sin previo -> no modelable
        ir::IrValueId nv;
        if (is_high) { // ah/bh/ch/dh -> bits 8..15
            const ir::IrValueId lo = BIN(ir::IrOp::SHL, and_mask(r, 8), K(8));
            const ir::IrValueId keep =
                BIN(ir::IrOp::AND, old, K((int64_t)~0xFF00ull));
            nv = BIN(ir::IrOp::OR, keep, lo);
        } else {
            const ir::IrValueId lo = and_mask(r, w);
            const ir::IrValueId keep =
                BIN(ir::IrOp::AND, old, K((int64_t)~width_mask(w)));
            nv = BIN(ir::IrOp::OR, keep, lo);
        }
        cur[canon] = nv;
        mark_write(canon);
    };

    // Lift instruccion a instruccion.  Cualquier forma no soportada -> false.
    // Indexado para poder mirar la SIGUIENTE instruccion (patrones fusionados
    // de 2 instrucciones que dependen de las flags: cmp + setcc).
    for (size_t ii = 0; ii < insns.size(); ++ii) {
        const std::string &insn = insns[ii];
        std::string m;
        std::vector<std::string> ops;
        split_insn(insn, m, ops);
        ir::IrOp bop;
        bool ok = true;

        // cmp/test a,b + setcc rd -> IrOp de comparacion (0/1).  El asm que
        // produce un booleano via flags se vuelve una comparacion tipada; las
        // flags no cruzan el par (patron cerrado).  cmp/test NO seguido de un
        // consumidor de flags modelado -> el par no encaja -> false (no
        // modelamos flags sueltas en straight-line).
        if ((m == "cmp" || m == "test") && ops.size() == 2 &&
            ii + 1 < insns.size()) {
            std::string m2;
            std::vector<std::string> ops2;
            split_insn(insns[ii + 1], m2, ops2);
            ir::IrOp cop;
            bool csigned = false;
            if (m2.size() >= 4 && m2.compare(0, 3, "set") == 0 &&
                ops2.size() == 1 &&
                setcc_to_cmp(m2.substr(3), cop, csigned)) {
                // Ancho de la comparacion = ancho del primer operando del cmp.
                std::string ac, bc, dc;
                bool ah = false, bh = false, dh = false;
                const int aw = reg_info(ops[0], ac, ah);
                const int dw = reg_info(ops2[0], dc, dh); // setcc: r/m8
                if (aw == 0 || dw == 0) return false; // solo reg-reg/imm
                // Para `test a,b` el x86 hace AND; aqui solo soportamos
                // `test r,r` con el MISMO registro (test r,r == cmp r,0).
                ir::IrValueId av, bv;
                if (m == "test") {
                    if (ops[0] != ops[1]) return false; // test r,r mismo reg
                    av = read_reg(ac, aw, ah, csigned, ok);
                    if (!ok) return false;
                    bv = K(0);
                } else {
                    av = read_reg(ac, aw, ah, csigned, ok);
                    if (!ok) return false;
                    bv = read_op(ops[1], aw, csigned, ok);
                    if (!ok) return false;
                }
                const ir::IrValueId res = BIN(cop, av, bv);
                // setcc escribe un byte (0/1); el resto del registro se
                // preserva -> write_reg a 8 bits.
                write_reg(dc, 8, dh, res, ok);
                if (!ok) return false;
                ++ii; // consumir el setcc
                continue;
            }
            // cmp/test sin setcc detras -> flags no modelables aqui.
            return false;
        }

        if (m == "mov" && ops.size() == 2) {
            const std::string dmem = plain_mem(ops[0]);
            const std::string smem = plain_mem(ops[1]);
            // Memoria con modo de direccionamiento no-plano ([base+idx*sc+disp])
            // aun no soportada -> fallback.
            if ((is_mem(ops[0]) && dmem.empty()) ||
                (is_mem(ops[1]) && smem.empty()))
                return false;
            if (!dmem.empty()) { // mov [rd], src  (memoria HOST)
                std::string sc;
                bool sh = false;
                const int srw = reg_info(ops[1], sc, sh);
                int w = mem_hint_width(ops[0]);
                if (w == 0) w = srw;      // ancho del store = registro fuente
                if (w == 0) return false; // `mov [r], imm` sin size-hint: ambiguo
                const ir::IrValueId a = get_full(dmem, ok);
                if (!ok) return false;
                const ir::IrValueId v = read_op(ops[1], w, false, ok);
                if (!ok) return false;
                emit_store(fn, block, v, a, w, /*host=*/true, line);
            } else if (!smem.empty()) { // mov rd, [rs]  (memoria HOST)
                std::string rc;
                bool rh = false;
                const int rw = reg_info(ops[0], rc, rh);
                if (rw == 0) return false;
                int w = mem_hint_width(ops[1]);
                if (w == 0) w = rw;       // ancho del load = registro destino
                const ir::IrValueId a = get_full(smem, ok);
                if (!ok) return false;
                const ir::IrValueId ld =
                    emit_load(fn, block, a, w, /*host=*/true, line);
                write_reg(rc, rw, rh, ld, ok);
                if (!ok) return false;
            } else { // mov rd, (reg|imm)
                std::string rc;
                bool rh = false;
                const int rw = reg_info(ops[0], rc, rh);
                if (rw == 0) return false;
                const ir::IrValueId v = read_op(ops[1], rw, false, ok);
                if (!ok) return false;
                write_reg(rc, rw, rh, v, ok);
                if (!ok) return false;
            }
        } else if (m == "lea" && ops.size() == 2) {
            std::string rc;
            bool rh = false;
            const int rw = reg_info(ops[0], rc, rh);
            const std::string src = plain_mem(ops[1]);
            if (rw == 0 || src.empty()) return false; // solo lea rd,[reg]
            const ir::IrValueId a = get_full(src, ok);
            if (!ok) return false;
            write_reg(rc, rw, rh, a, ok);
            if (!ok) return false;
        } else if (binop_of(m, bop) && ops.size() == 2) {
            std::string rc;
            bool rh = false;
            const int rw = reg_info(ops[0], rc, rh);
            if (rw == 0) return false; // dst debe ser registro
            const bool is_shift = (bop == ir::IrOp::SHL || bop == ir::IrOp::SHR ||
                                   bop == ir::IrOp::SAR);
            // dst leido: con signo solo para SAR (necesita el bit de signo al
            // ancho); el resto sin signo (los bits altos se enmascaran al
            // escribir).
            const ir::IrValueId a =
                read_reg(rc, rw, rh, bop == ir::IrOp::SAR, ok);
            if (!ok) return false;
            ir::IrValueId b;
            if (is_shift) {
                // La cuenta se enmascara por el ancho (x86: & 0x1F para <=32
                // bits, & 0x3F para 64).  Si es INMEDIATA, la plegamos a una
                // constante (evita un shift-por-valor y da mejor IR); si es un
                // registro (cl), enmascaramos en runtime.
                const int64_t cmask = (rw >= 64 ? 63 : 31);
                int64_t cimm = 0;
                if (parse_imm(ops[1], cimm)) {
                    b = K(cimm & cmask);
                } else {
                    b = read_op(ops[1], 64, false, ok);
                    if (!ok) return false;
                    b = BIN(ir::IrOp::AND, b, K(cmask));
                }
            } else {
                b = read_op(ops[1], rw, false, ok);
                if (!ok) return false;
            }
            const ir::IrValueId res = BIN(bop, a, b);
            write_reg(rc, rw, rh, res, ok);
            if (!ok) return false;
        } else if ((m == "neg" || m == "not") && ops.size() == 1) {
            std::string rc;
            bool rh = false;
            const int rw = reg_info(ops[0], rc, rh);
            if (rw == 0) return false;
            const ir::IrValueId a = read_reg(rc, rw, rh, false, ok);
            if (!ok) return false;
            const ir::IrValueId res = emit_un(
                fn, block, m == "neg" ? ir::IrOp::NEG : ir::IrOp::NOT, a, line);
            write_reg(rc, rw, rh, res, ok);
            if (!ok) return false;
        } else if ((m == "inc" || m == "dec") && ops.size() == 1) {
            std::string rc;
            bool rh = false;
            const int rw = reg_info(ops[0], rc, rh);
            if (rw == 0) return false;
            const ir::IrValueId a = read_reg(rc, rw, rh, false, ok);
            if (!ok) return false;
            const ir::IrValueId res =
                BIN(m == "inc" ? ir::IrOp::ADD : ir::IrOp::SUB, a, K(1));
            write_reg(rc, rw, rh, res, ok);
            if (!ok) return false;
        } else if ((m == "popcnt" || m == "lzcnt" || m == "tzcnt") &&
                   ops.size() == 2) {
            /* popcnt/lzcnt/tzcnt rd, rs -> POPCNT/CLZ/CTZ.  Solo 64 bits (donde
             * el mapeo es EXACTO; a 32/16 el resultado depende del ancho ->
             * incremento posterior, hoy cae a ASM_MICRO). */
            std::string rc, sc;
            bool rh = false, sh = false;
            const int rw = reg_info(ops[0], rc, rh);
            const int sw = reg_info(ops[1], sc, sh);
            if (rw != 64 || sw != 64) return false;
            const ir::IrValueId a = read_reg(sc, 64, sh, false, ok);
            if (!ok) return false;
            const ir::IrOp uop = (m == "popcnt")  ? ir::IrOp::POPCNT
                                 : (m == "lzcnt") ? ir::IrOp::CLZ
                                                  : ir::IrOp::CTZ;
            const ir::IrValueId res = emit_un(fn, block, uop, a, line);
            write_reg(rc, 64, rh, res, ok);
            if (!ok) return false;
        } else if (m == "bswap" && ops.size() == 1) {
            /* bswap rd -> BYTESWAP (in-place).  Solo 64 bits (bswap de 32 bits
             * invertiria 4 bytes, distinto ancho). */
            std::string rc;
            bool rh = false;
            const int rw = reg_info(ops[0], rc, rh);
            if (rw != 64) return false;
            const ir::IrValueId a = read_reg(rc, 64, rh, false, ok);
            if (!ok) return false;
            const ir::IrValueId res =
                emit_un(fn, block, ir::IrOp::BYTESWAP, a, line);
            write_reg(rc, 64, rh, res, ok);
            if (!ok) return false;
        } else if ((m == "movzx" || m == "movsx" || m == "movsxd") &&
                   ops.size() == 2) {
            /* Extension de ancho: lee rs a SU ancho (zero-extend en movzx,
             * sign-extend en movsx/movsxd) y lo escribe en rd.  El modelo de
             * anchos ya cubre exactamente la semantica x86.  Solo reg-reg
             * (memoria -> host, camino opaco). */
            if (is_mem(ops[1])) return false;
            std::string rc, sc;
            bool rh = false, sh = false;
            const int rw = reg_info(ops[0], rc, rh);
            const int sw = reg_info(ops[1], sc, sh);
            if (rw == 0 || sw == 0) return false;
            const bool sgn = (m != "movzx");
            const ir::IrValueId v = read_reg(sc, sw, sh, sgn, ok);
            if (!ok) return false;
            write_reg(rc, rw, rh, v, ok);
            if (!ok) return false;
        } else {
            return false; // instruccion fuera del subset -> INLINE_ASM
        }
    }

    // Escribir de vuelta cada registro LIGADO escrito, al ancho de su var (el
    // valor arquitectonico completo de 64 bits truncado al slot).
    for (const std::string &r : wrote) {
        auto b = bound.find(r);
        auto v = cur.find(r);
        if (b != bound.end() && v != cur.end()) {
            const bool slot_host = fn.values[b->second.slot].is_host_ptr;
            emit_store(fn, block, v->second, b->second.slot, b->second.width_bits,
                       slot_host, line);
        }
    }
    return true;
}

} // namespace

bool asm_lift_general(
    ir::IrFunction &fn, uint32_t block, instr_db::Isa isa,
    const std::string &body,
    const std::unordered_map<std::string, AsmBoundReg> &bound, uint32_t line) {
    /* Dispatch por ISA.  Cada arquitectura aporta su frontend (mnemonicos +
     * parsing + semantica de registro parcial) y baja al MISMO IR neutro.
     * arm64/arm32/riscv: anadir su lift_<isa> reusando el core de emision. */
    switch (isa) {
    case instr_db::Isa::X86: return lift_x86(fn, block, body, bound, line);
    default: return false; // ISA sin lifter aun -> el llamador cae a ASM_MICRO
    }
}

} // namespace vx
