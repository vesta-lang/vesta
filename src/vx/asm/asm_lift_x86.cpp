/* * VestaVM -- frontend x86 del lift de asm a IR.
 * Copyright (C) 2026 David Lopez.T (DesmonHak).  GPLv2 + excepcion de runtime. */

/** @file vx/asm/asm_lift_x86.cpp
 *  @brief Frontend x86/x86-64 del lift de asm inline a IR neutro.  Reconoce el
 *  subset (mnemonicos + registros + direccionamiento x86) y lo baja al IR via el
 *  core NEUTRO (@ref asm_lift_core.h: emisores, register-file, driver del CFG).
 *  Ver asm_lift_x86.h. */
#include "vx/asm/asm_lift_x86.h"

#include "vx/asm/asm_cfg.h"       // build_asm_cfg
#include "vx/asm/asm_effects.h"   // asm_canonical_reg
#include "vx/asm/asm_lift_core.h" // core neutro
#include "vx/asm/asm_phys_reg.h"  // asm_x86_gp_index

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace vx {

// Traer al ambito los helpers NEUTROS del core (emisores, utilidades de texto,
// LiftCtx / CfgHooks / lift_cfg_neutral).  El frontend x86 los usa por nombre.
using namespace vx::asmlift;

namespace {

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

} // namespace

/** @brief Lifter x86/x86-64: reconoce el subset entero y lo baja a IR neutro.
 *  Straight-line (1 bloque) o con RAMAS: construye el CFG del asm y lo baja a
 *  IR-CFG via el driver NEUTRO (@ref lift_cfg_neutral), aportando los hooks x86
 *  (lift de instrucciones + condicion de rama).  reg_info/binop_of/parse_mem/
 *  mem_hint_width/setcc_to_cmp son la parte x86.  @p out_exit (si != null) queda
 *  con el bloque de continuacion (donde sigue el codigo tras el asm). */
bool lift_x86(
    ir::IrFunction &fn, uint32_t block, const std::string &body,
    const std::unordered_map<std::string, AsmBoundReg> &bound, uint32_t line,
    uint32_t *out_exit) {
    // El CFG (por-ISA) trocea el body en bloques basicos + aristas.  Usamos su
    // lista de instrucciones (mismo troceo que instructions()) para indexar los
    // bloques de forma consistente.  Anadimos un `nop` centinela al final: una
    // etiqueta de SALIDA colocada al final (idioma comun `... jCC end; ...;
    // end:`) queda sin instruccion a la que adjuntarse -> el nop la ancla y el
    // salto se resuelve a ese bloque (que cae a la continuacion).
    const std::string body_cfg = body + "\nnop";
    const vx::AsmCfg cfg = vx::build_asm_cfg(vx::instr_db::Isa::X86, body_cfg);
    std::vector<std::string> insns;
    insns.reserve(cfg.insns.size());
    for (const auto &in : cfg.insns) insns.push_back(in.text);
    if (insns.empty()) return false;
    // Ramas no resueltas / terminadores desconocidos -> no liftamos (opaco).
    if (cfg.has_unresolved_target || !cfg.unknown_terminators.empty())
        return false;

    // Estado SSA por registro: cur[canon] = valor ARQUITECToNICO COMPLETO de
    // 64 bits del registro fisico (rax), con los bits altos correctos segun las
    // reglas de x86.  wrote = registros LIGADOS escritos (para el flush).
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

    // HOOK x86: lifta las instrucciones insns[from, to).  Indexado para mirar la
    // SIGUIENTE instruccion (patrones fusionados de 2 instr que dependen de las
    // flags: cmp+setcc / cmp+cmovcc).  El look-ahead NO cruza `to` (frontera de
    // bloque en el camino CFG).  Cualquier forma no soportada -> false.
    auto lift_range = [&](size_t from, size_t to) -> bool {
    for (size_t ii = from; ii < to; ++ii) {
        const std::string &insn = insns[ii];
        std::string m;
        std::vector<std::string> ops;
        split_insn(insn, m, ops);
        ir::IrOp bop;
        bool ok = true;

        // cmp/test a,b + (setcc rd | cmovcc rd,rs): patron fusionado que
        // consume las flags EN EL PAR -> comparacion tipada (setcc) o select
        // branchless (cmov).  Las flags no cruzan el par.  cmp/test sin un
        // consumidor de flags modelado detras -> el par no encaja -> false (no
        // modelamos flags sueltas en straight-line).
        if ((m == "cmp" || m == "test") && ops.size() == 2 &&
            ii + 1 < to) {
            std::string m2;
            std::vector<std::string> ops2;
            split_insn(insns[ii + 1], m2, ops2);
            ir::IrOp cop;
            bool csigned = false;
            bool is_set = false, is_cmov = false;
            if (m2.size() >= 4 && m2.compare(0, 3, "set") == 0 &&
                ops2.size() == 1)
                is_set = setcc_to_cmp(m2.substr(3), cop, csigned);
            if (!is_set && m2.size() > 4 && m2.compare(0, 4, "cmov") == 0 &&
                ops2.size() == 2)
                is_cmov = setcc_to_cmp(m2.substr(4), cop, csigned);
            if (!is_set && !is_cmov) return false;

            // cond = (a <cc> b), leyendo a/b al ancho del cmp con el signo del cc.
            std::string ac;
            bool ah = false;
            const int aw = reg_info(ops[0], ac, ah);
            if (aw == 0) return false;
            ir::IrValueId av, bv;
            if (m == "test") {
                // test r,r == cmp r,0 (solo el mismo registro).
                if (ops[0] != ops[1]) return false;
                av = read_reg(ac, aw, ah, csigned, ok);
                if (!ok) return false;
                bv = K(0);
            } else {
                av = read_reg(ac, aw, ah, csigned, ok);
                if (!ok) return false;
                bv = read_op(ops[1], aw, csigned, ok);
                if (!ok) return false;
            }
            const ir::IrValueId cond = BIN(cop, av, bv); // 0/1

            if (is_set) {
                // setcc rd: byte 0/1; resto del registro preservado.
                std::string dc;
                bool dh = false;
                if (reg_info(ops2[0], dc, dh) == 0) return false;
                write_reg(dc, 8, dh, cond, ok);
                if (!ok) return false;
            } else {
                // cmovcc rd, rs: rd = cond ? (rs escrito al ancho) : rd.  Select
                // BRANCHLESS (el lifter no emite ramas): mask = -(cond) (0 o -1);
                // rd = rd_old ^ ((rd_old ^ taken) & mask).  Respeta la asimetria
                // de cmov: taken de 32 bits zero-extiende; not-taken preserva TODO.
                if (is_mem(ops2[1])) return false; // solo reg
                std::string dc, sc;
                bool dh = false, sh = false;
                const int dw = reg_info(ops2[0], dc, dh);
                const int sw = reg_info(ops2[1], sc, sh);
                if (dw == 0 || sw == 0 || dw != sw) return false;
                const ir::IrValueId rd_old = get_full(dc, ok);
                if (!ok) return false;
                const ir::IrValueId rs = read_reg(sc, dw, sh, false, ok);
                if (!ok) return false;
                // taken = rs escrito en rd al ancho de cmov.
                ir::IrValueId taken;
                if (dw >= 32) {
                    taken = rs; // low-dw zero-extendido = escritura de 32/64
                } else {
                    taken = BIN(ir::IrOp::OR,
                                BIN(ir::IrOp::AND, rd_old,
                                    K((int64_t)~width_mask(dw))),
                                rs);
                }
                const ir::IrValueId mask =
                    emit_un(fn, block, ir::IrOp::NEG, cond, line);
                const ir::IrValueId diff = BIN(ir::IrOp::XOR, rd_old, taken);
                cur[dc] = BIN(ir::IrOp::XOR, rd_old,
                              BIN(ir::IrOp::AND, diff, mask));
                mark_write(dc);
            }
            ++ii; // consumir el setcc/cmov
            continue;
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
        } else if ((m == "rol" || m == "ror") && ops.size() == 2) {
            // Rotacion 64-bit -> ROTL/ROTR (el backend la emite nativa: cuenta
            // constante -> rol/ror imm; en registro -> rol/ror CL).
            std::string rc;
            bool rh = false;
            const int rw = reg_info(ops[0], rc, rh);
            if (rw != 64) return false; // solo 64-bit (mapeo exacto)
            const ir::IrValueId a = read_reg(rc, 64, rh, false, ok);
            if (!ok) return false;
            const ir::IrValueId cnt = read_op(ops[1], 64, false, ok);
            if (!ok) return false;
            const ir::IrValueId res =
                BIN(m == "rol" ? ir::IrOp::ROTL : ir::IrOp::ROTR, a, cnt);
            write_reg(rc, 64, rh, res, ok);
            if (!ok) return false;
        } else if (m == "cqo" || m == "cqto" || m == "cdq" || m == "cdqe") {
            // Extension del dividendo a rdx:rax (sign/zero) previa a idiv/div.
            // El patron div/idiv de abajo la ABSORBE (la division 64/64 modela
            // el dividendo alto): aqui es un no-op.
        } else if ((m == "div" || m == "idiv") && ops.size() == 1) {
            // Division 64/64: rax = cociente, rdx = resto.  Exige el setup del
            // dividendo alto INMEDIATAMENTE antes -- `xor rdx,rdx` (div, sin
            // signo) o `cqo/cdq` (idiv, con signo) -- para garantizar que el
            // dividendo es rax de 64 bits (no rdx:rax de 128).  El divisor debe
            // ser un registro de 64 bits.
            if (is_mem(ops[0])) return false;
            std::string dc;
            bool dh = false;
            const int dw = reg_info(ops[0], dc, dh);
            if (dw != 64) return false;
            if (ii <= from) return false; // sin instruccion previa en el bloque
            std::string pm;
            std::vector<std::string> pops;
            split_insn(insns[ii - 1], pm, pops);
            const bool is_signed = (m == "idiv");
            bool setup_ok = false;
            if (is_signed) {
                setup_ok = (pm == "cqo" || pm == "cqto" || pm == "cdq");
            } else if (pm == "xor" && pops.size() == 2) {
                // xor rdx,rdx | xor edx,edx (rdx = 0).
                const std::string p0 = asm_canonical_reg(pops[0]);
                const std::string p1 = asm_canonical_reg(pops[1]);
                setup_ok = (p0 == "rdx" && p1 == "rdx");
            }
            if (!setup_ok) return false; // dividendo alto desconocido -> opaco
            const ir::IrValueId dvd = read_reg("rax", 64, false, false, ok);
            if (!ok) return false;
            const ir::IrValueId dsr = read_reg(dc, 64, dh, false, ok);
            if (!ok) return false;
            const ir::IrType ty = is_signed ? ir::IrType::I64 : ir::IrType::U64;
            // Leer el dividendo UNA vez y sacar cociente + resto de el (antes de
            // pisar rax).
            const ir::IrValueId q =
                emit_bin_ty(fn, block, ir::IrOp::DIV, dvd, dsr, ty, line);
            const ir::IrValueId r =
                emit_bin_ty(fn, block, ir::IrOp::MOD, dvd, dsr, ty, line);
            write_reg("rax", 64, false, q, ok);
            if (!ok) return false;
            write_reg("rdx", 64, false, r, ok);
            if (!ok) return false;
        } else if (m == "nop") {
            // no-op (incluido el centinela de etiqueta final): no emite IR.
        } else {
            return false; // instruccion fuera del subset -> INLINE_ASM
        }
    }
    return true;
    }; // fin del hook lift_range

    // --- 1 bloque (straight-line, sin ramas): liftar todo + flush final. ---
    if (cfg.blocks.size() <= 1) {
        if (!lift_range(0, insns.size())) return false;
        for (const std::string &r : wrote) {
            auto b = bound.find(r);
            auto v = cur.find(r);
            if (b != bound.end() && v != cur.end()) {
                const bool slot_host = fn.values[b->second.slot].is_host_ptr;
                emit_store(fn, block, v->second, b->second.slot,
                           b->second.width_bits, slot_host, line);
            }
        }
        if (out_exit) *out_exit = block;
        return true;
    }

    // --- Con RAMAS: CFG del asm -> IR-CFG via el driver NEUTRO + hooks x86. ---
    LiftCtx ctx{fn, block, line, bound, cur, wrote};
    CfgHooks hooks;
    hooks.lift_range = [&](size_t from, size_t to) {
        return lift_range(from, to);
    };
    hooks.term_start = [&](const vx::AsmBasicBlock &bb) -> uint32_t {
        // x86: CondBranch -> el cmp (last-1, lo maneja branch_cond); UncondJump
        // -> el jmp (last, no es cuerpo); resto -> last+1 (todo es cuerpo).
        if (bb.term == vx::AsmTerm::CondBranch)
            return (bb.last > bb.first) ? bb.last - 1u : bb.first;
        if (bb.term == vx::AsmTerm::UncondJump) return bb.last;
        return bb.last + 1u;
    };
    hooks.branch_cond = [&](const vx::AsmBasicBlock &bb) -> ir::IrValueId {
        // Terminador esperado: `cmp/test a,b` (last-1) + `jCC target` (last).
        if (bb.last < 1u) return ir::IR_NO_VALUE;
        const size_t ci = bb.last - 1u;
        if (ci < bb.first) return ir::IR_NO_VALUE;
        std::string cm;
        std::vector<std::string> cops;
        split_insn(insns[ci], cm, cops);
        if ((cm != "cmp" && cm != "test") || cops.size() != 2)
            return ir::IR_NO_VALUE;
        std::string jm;
        std::vector<std::string> jops;
        split_insn(insns[bb.last], jm, jops);
        if (jm.size() < 2 || jm[0] != 'j') return ir::IR_NO_VALUE;
        ir::IrOp cop;
        bool csigned = false;
        if (!setcc_to_cmp(jm.substr(1), cop, csigned)) return ir::IR_NO_VALUE;
        bool ok = true;
        std::string ac;
        bool ah = false;
        const int aw = reg_info(cops[0], ac, ah);
        if (aw == 0) return ir::IR_NO_VALUE;
        ir::IrValueId av, bv;
        if (cm == "test") {
            if (cops[0] != cops[1]) return ir::IR_NO_VALUE;
            av = read_reg(ac, aw, ah, csigned, ok);
            if (!ok) return ir::IR_NO_VALUE;
            bv = K(0);
        } else {
            av = read_reg(ac, aw, ah, csigned, ok);
            if (!ok) return ir::IR_NO_VALUE;
            bv = read_op(cops[1], aw, csigned, ok);
            if (!ok) return ir::IR_NO_VALUE;
        }
        return BIN(cop, av, bv);
    };
    uint32_t exit_blk = block;
    if (!lift_cfg_neutral(ctx, cfg, hooks, exit_blk)) return false;
    if (out_exit) *out_exit = exit_blk;
    return true;
}

} // namespace vx
