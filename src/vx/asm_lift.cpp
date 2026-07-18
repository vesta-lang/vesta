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
 * @file asm_lift.cpp
 * @brief Implementacion del reconocimiento de patrones de asm liftables a IR.
 */

#include "vx/asm_lift.h"

#include "vx/asm_effects.h" // asm_canonical_reg

#include <cctype>
#include <string>
#include <vector>

namespace vx {

namespace {

std::string lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

std::string strip_comment(const std::string &s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ';')
            return s.substr(0, i);
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
            return s.substr(0, i);
    }
    return s;
}

/// ¿La linea es una etiqueta (identificador seguido de ':')?
bool is_label(const std::string &line) {
    const std::string t = trim(line);
    size_t colon = t.find(':');
    if (colon == std::string::npos)
        return false;
    for (size_t i = 0; i < colon; ++i)
        if (!(std::isalnum(static_cast<unsigned char>(t[i])) || t[i] == '_' ||
              t[i] == '.'))
            return false;
    return colon > 0;
}

/// Instrucciones reales (sin comentarios/labels/vacias).
std::vector<std::string> real_insns(const std::string &body) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        std::string raw = body.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        std::string t = trim(strip_comment(raw));
        if (t.empty() || is_label(t))
            continue;
        out.push_back(t);
    }
    return out;
}

/// Trocea una instruccion en (mnemonico, [operandos]).  Respeta @c [..] (no
/// parte por comas dentro de corchetes).
void split_insn(const std::string &insn, std::string &mnem,
                std::vector<std::string> &ops) {
    mnem.clear();
    ops.clear();
    size_t i = 0;
    while (i < insn.size() && std::isspace(static_cast<unsigned char>(insn[i])))
        ++i;
    size_t j = i;
    while (j < insn.size() && !std::isspace(static_cast<unsigned char>(insn[j])))
        ++j;
    mnem = lower(insn.substr(i, j - i));
    // Operandos: partir por comas fuera de [..].
    std::string cur;
    int depth = 0;
    for (size_t k = j; k < insn.size(); ++k) {
        char c = insn[k];
        if (c == '[')
            ++depth;
        else if (c == ']')
            --depth;
        if (c == ',' && depth == 0) {
            std::string o = trim(cur);
            if (!o.empty())
                ops.push_back(o);
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::string o = trim(cur);
    if (!o.empty())
        ops.push_back(o);
}

/// Si @p op es una memoria @c [reg] PLANA (un solo registro, sin
/// desplazamiento/indice), devuelve el registro canonico; si no, "".
std::string plain_mem_reg(const std::string &op) {
    if (op.size() < 3 || op.front() != '[' || op.back() != ']')
        return "";
    std::string inner = trim(op.substr(1, op.size() - 2));
    // Rechazar cualquier operador de direccionamiento.
    for (char c : inner)
        if (c == '+' || c == '-' || c == '*' || c == ' ' || c == ':')
            return "";
    return asm_canonical_reg(inner);
}

AsmLift lift_x86(const std::vector<std::string> &insns) {
    AsmLift r;
    // Incremento 1: forma de UNA instruccion (el setup de expected/desired lo
    // aportan los register() bindings).
    if (insns.size() != 1) {
        r.note = "solo se lifta la forma de una instruccion (lock cmpxchg/xadd)";
        return r;
    }
    std::string mnem;
    std::vector<std::string> ops;
    split_insn(insns[0], mnem, ops);

    // Requiere el prefijo `lock` (atomicidad cross-core).  split_insn lo trata
    // como mnemonico si va pegado; lo normalizamos: si mnem=="lock", el
    // verdadero mnemonico es el primer operando-token... mejor detectarlo aqui.
    bool has_lock = false;
    if (mnem == "lock") {
        has_lock = true;
        // Re-trocear quitando el "lock ".
        const std::string rest = trim(insns[0].substr(4));
        split_insn(rest, mnem, ops);
    }
    if (!has_lock) {
        r.note = "cmpxchg/xadd sin prefijo lock no es atomico cross-core";
        return r;
    }

    if (mnem == "cmpxchg") {
        // cmpxchg [addr], desired ; expected/old implicitos en rax.
        if (ops.size() != 2) {
            r.note = "cmpxchg: se esperan 2 operandos";
            return r;
        }
        const std::string addr = plain_mem_reg(ops[0]);
        if (addr.empty()) {
            r.note = "cmpxchg: el 1er operando debe ser [reg] plano";
            return r;
        }
        instr_db::ParsedOp des = instr_db::parse_operand(instr_db::Isa::X86, ops[1]);
        if (des.kind != instr_db::OP_REG || des.width != 64) {
            r.note = "cmpxchg: solo i64 (registro de 64 bits) soportado";
            return r;
        }
        r.op = AsmLiftOp::AtomicCas;
        r.addr_reg = addr;
        r.des_reg = asm_canonical_reg(ops[1]);
        r.exp_reg = "rax";    // expected implicito
        r.result_reg = "rax"; // old value queda en rax
        r.width = 64;
        return r;
    }

    if (mnem == "xadd") {
        // xadd [addr], src ; src recibe el valor viejo (fetch-and-add).
        if (ops.size() != 2) {
            r.note = "xadd: se esperan 2 operandos";
            return r;
        }
        const std::string addr = plain_mem_reg(ops[0]);
        if (addr.empty()) {
            r.note = "xadd: el 1er operando debe ser [reg] plano";
            return r;
        }
        instr_db::ParsedOp src = instr_db::parse_operand(instr_db::Isa::X86, ops[1]);
        if (src.kind != instr_db::OP_REG || src.width != 64) {
            r.note = "xadd: solo i64 (registro de 64 bits) soportado";
            return r;
        }
        r.op = AsmLiftOp::AtomicAdd;
        r.addr_reg = addr;
        r.des_reg = asm_canonical_reg(ops[1]); // delta
        r.result_reg = r.des_reg;              // old value queda en el mismo reg
        r.width = 64;
        return r;
    }

    r.note = "mnemonico no liftible (" + mnem + ")";
    return r;
}

} // namespace

AsmLift asm_lift_detect(instr_db::Isa isa, const std::string &body) {
    std::vector<std::string> insns = real_insns(body);
    if (insns.empty()) {
        AsmLift r;
        r.note = "bloque vacio";
        return r;
    }
    switch (isa) {
    case instr_db::Isa::X86:
        return lift_x86(insns);
    default:
        break;
    }
    AsmLift r;
    r.note = "lift no soportado para esta ISA todavia";
    return r;
}

} // namespace vx
