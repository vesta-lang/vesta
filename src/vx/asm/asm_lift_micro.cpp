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
 * @file vx/asm/asm_lift_micro.cpp
 * @brief Lift de instrucciones asm opacas SIN operandos de registro a
 *        @c IrOp::ASM_MICRO.  Ver vx/asm/asm_lift_micro.h.
 */

#include "vx/asm/asm_lift_micro.h"

#include "ir/ssa_ir.h"

#include <cctype>
#include <string>
#include <vector>

namespace vx {

namespace {

/// Recorta espacios de los extremos.
std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

/// Divide el cuerpo en instrucciones (una por linea), descartando comentarios
/// (@c // y @c ;) y etiquetas (@c "name:").
std::vector<std::string> instructions(const std::string &body) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        std::string ln = body.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        // Quitar comentario // o ;
        size_t c = ln.find("//");
        if (c != std::string::npos) ln.resize(c);
        c = ln.find(';');
        if (c != std::string::npos) ln.resize(c);
        ln = trim(ln);
        if (!ln.empty() && ln.back() != ':') // no etiqueta
            out.push_back(ln);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

/// Empaqueta los efectos de la DB en el byte @c eff de @ref ir::AsmMicro:
/// bit0 memoria, bit1 lee flags, bit2 escribe flags, bit3 barrera.
uint8_t pack_eff(const instr_db::AsmInsnSem &sem) {
    uint8_t e = 0;
    if (sem.reads_mem || sem.writes_mem) e |= 0x1;
    if (sem.reads_flags) e |= 0x2;
    if (sem.writes_flags) e |= 0x4;
    if (sem.barrier) e |= 0x8;
    return e;
}

} // namespace

bool asm_lift_micro(ir::IrFunction &fn, uint32_t block, instr_db::Isa isa,
                    const std::string &body, uint32_t line) {
    const std::vector<std::string> insns = instructions(body);
    if (insns.empty()) return false;

    // Microarq para la semantica: solo usamos los campos SEMaNTICOS (form_id,
    // barrera, mem, flags, reads/writes), no la latencia, asi que cualquier
    // microarq valida de la ISA sirve.  Skylake para x86; fallback 0.
    int32_t ua = instr_db::microarch_by_name(isa, "intel-skylake");
    if (ua < 0) ua = 0;

    // Fase 1 (validacion transaccional): TODAS las instrucciones deben ser
    // formas conocidas por la DB y SIN operandos de registro (reads/writes de
    // registro vacios).  Cualquier fallo -> el bloque no es del subset ASM_MICRO
    // sin operandos -> false (el llamador emite INLINE_ASM).
    std::vector<instr_db::AsmInsnSem> sems;
    sems.reserve(insns.size());
    for (const std::string &insn : insns) {
        instr_db::AsmInsnSem sem =
            instr_db::asm_insn_sem(isa, insn, (uint32_t)ua);
        if (sem.form_id < 0)          // desconocida por la DB
            return false;
        if (!sem.reads.empty() || !sem.writes.empty())
            return false;             // tiene operandos de registro (otro inc.)
        sems.push_back(std::move(sem));
    }

    // Fase 2 (emision): una ASM_MICRO por instruccion.
    for (size_t i = 0; i < insns.size(); ++i) {
        ir::AsmMicro am;
        am.isa = (uint8_t)isa;
        am.form_id = (uint32_t)sems[i].form_id;
        am.tmpl = insns[i];
        am.eff = pack_eff(sems[i]);
        // ins/outs vacios: sin operandos de registro que enhebrar.

        ir::IrInstr in{};
        in.op = ir::IrOp::ASM_MICRO;
        in.type = ir::IrType::VOID;
        in.dst = ir::IR_NO_VALUE;
        in.imm = fn.asm_micros.size();
        in.source_line = line;
        fn.asm_micros.push_back(std::move(am));
        fn.append(block, std::move(in));
    }
    return true;
}

} // namespace vx
