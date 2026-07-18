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
 * @file asm_diag.cpp
 * @brief Implementacion de los diagnosticos estructurales del CFG de asm.
 */

#include "vx/asm_diag.h"

#include <deque>
#include <vector>

namespace vx {

namespace {

/// Marca los bloques ALCANZABLES desde @p start siguiendo @p succs (BFS).
std::vector<bool> reachable_forward(const AsmCfg &cfg, uint32_t start) {
    std::vector<bool> seen(cfg.blocks.size(), false);
    if (cfg.blocks.empty())
        return seen;
    std::deque<uint32_t> q;
    seen[start] = true;
    q.push_back(start);
    while (!q.empty()) {
        uint32_t b = q.front();
        q.pop_front();
        for (uint32_t s : cfg.blocks[b].succs)
            if (!seen[s]) {
                seen[s] = true;
                q.push_back(s);
            }
    }
    return seen;
}

/// ¿El bloque @p b es una SALIDA del bloque asm?  Es salida si retorna, si su
/// destino es indirecto/desconocido (conservador: podria salir, para NO reportar
/// un bucle falso), o si el control puede CAER FUERA del bloque asm: cuando el
/// terminador admite fallthrough (fallthrough/call/rama condicional) y no hay
/// ninguna instruccion despues (una `jnz .loop` al final cae fuera cuando la
/// condicion es falsa).
bool is_exit_block(const AsmCfg &cfg, uint32_t b) {
    const AsmBasicBlock &bb = cfg.blocks[b];
    if (bb.term == AsmTerm::Ret || bb.term == AsmTerm::Indirect ||
        bb.term == AsmTerm::Unknown)
        return true;
    const bool fallthrough_possible =
        (bb.term == AsmTerm::Fallthrough || bb.term == AsmTerm::Call ||
         bb.term == AsmTerm::CondBranch);
    const bool at_end = (bb.last + 1 >= cfg.insns.size());
    return fallthrough_possible && at_end;
}

/// Marca los bloques que PUEDEN ALCANZAR alguna salida (BFS inverso desde las
/// salidas siguiendo @p preds).
std::vector<bool> can_reach_exit(const AsmCfg &cfg) {
    std::vector<bool> can(cfg.blocks.size(), false);
    std::deque<uint32_t> q;
    for (uint32_t b = 0; b < cfg.blocks.size(); ++b)
        if (is_exit_block(cfg, b)) {
            can[b] = true;
            q.push_back(b);
        }
    while (!q.empty()) {
        uint32_t b = q.front();
        q.pop_front();
        for (uint32_t p : cfg.blocks[b].preds)
            if (!can[p]) {
                can[p] = true;
                q.push_back(p);
            }
    }
    return can;
}

} // namespace

std::vector<AsmDiag> asm_diagnose_cfg(const AsmCfg &cfg) {
    std::vector<AsmDiag> out;
    if (cfg.blocks.empty())
        return out;

    const std::vector<bool> reach = reachable_forward(cfg, 0);

    // 1) Codigo muerto: bloque no alcanzable desde la entrada.  Un solo aviso por
    //    bloque, en su primera instruccion.
    for (uint32_t b = 0; b < cfg.blocks.size(); ++b) {
        if (reach[b])
            continue;
        const AsmInsn &in = cfg.insns[cfg.blocks[b].first];
        AsmDiag d;
        d.severity = AsmDiagSeverity::Warning;
        d.line_no = in.line_no;
        d.code = "VXA001";
        d.message = "codigo muerto: instruccion inalcanzable en el bloque asm";
        out.push_back(std::move(d));
    }

    // 2) Salto a etiqueta no definida en el bloque.
    for (const AsmInsn &in : cfg.insns) {
        if ((in.term == AsmTerm::UncondJump || in.term == AsmTerm::CondBranch) &&
            !in.target.empty()) {
            bool found = false;
            for (const AsmInsn &d : cfg.insns)
                for (const std::string &l : d.labels)
                    if (l == in.target) {
                        found = true;
                        break;
                    }
            if (!found) {
                AsmDiag d;
                d.severity = AsmDiagSeverity::Warning;
                d.line_no = in.line_no;
                d.code = "VXA002";
                d.message = "salto a etiqueta '" + in.target +
                            "' no definida en el bloque asm";
                out.push_back(std::move(d));
            }
        }
    }

    // 3) Bucle sin salida: bloque alcanzable desde la entrada que NO puede llegar
    //    a ninguna salida.  Reportamos una vez por region (el bloque de menor
    //    indice del grupo alcanzable de bloques sin salida).
    const std::vector<bool> can = can_reach_exit(cfg);
    bool reported_loop = false;
    for (uint32_t b = 0; b < cfg.blocks.size(); ++b) {
        if (reach[b] && !can[b]) {
            if (!reported_loop) {
                const AsmInsn &in = cfg.insns[cfg.blocks[b].first];
                AsmDiag d;
                d.severity = AsmDiagSeverity::Warning;
                d.line_no = in.line_no;
                d.code = "VXA003";
                d.message =
                    "bucle sin salida: el flujo no puede abandonar el bloque asm";
                out.push_back(std::move(d));
                reported_loop = true;
            }
        }
    }

    return out;
}

std::vector<AsmDiag> asm_diagnose(instr_db::Isa isa, const std::string &body) {
    return asm_diagnose_cfg(build_asm_cfg(isa, body));
}

} // namespace vx
