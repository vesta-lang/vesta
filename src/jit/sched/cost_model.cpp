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
 * @file jit/sched/cost_model.cpp
 * @brief Implementacion del modelo de coste generico (core OoO moderno).
 */

#include "jit/sched/cost_model.h"

namespace jit {
namespace sched {

namespace {

/// ¿Alguno de los operandos toca MEMORIA? (load/store fusionado en la op).
bool touches_mem(const MInstr &mi) {
    return mi.dst.kind == MOperandKind::MEM ||
           mi.src1.kind == MOperandKind::MEM ||
           mi.src2.kind == MOperandKind::MEM;
}
/// ¿El destino es memoria? (la op ESCRIBE a memoria -> store).
bool writes_mem(const MInstr &mi) {
    return mi.dst.kind == MOperandKind::MEM;
}

} // namespace

InstrCost generic_cost_for(const MInstr &mi) {
    InstrCost c;
    switch (mi.op) {
    /* --- Pseudo-ops / barreras (sin coste de ejecucion) --- */
    case MOp::NOP:
    case MOp::LABEL_DEF:
    case MOp::COMMENT:
    case MOp::ARG:
        c.kind = ExecKind::OTHER;
        c.latency = 0.0f;
        c.recip_tp = 0.0f;
        return c;
    case MOp::CALL:
    case MOp::CALL_ABS:
    case MOp::RET:
    case MOp::SAFEPOINT:
        // Barrera: no reordenar a traves (efectos + clobber de caller-saved).
        c.kind = ExecKind::OTHER;
        c.latency = 1.0f;
        c.recip_tp = 1.0f;
        c.is_barrier = true;
        return c;

    /* --- Control de flujo --- */
    case MOp::JMP:
    case MOp::JCC:
        c.kind = ExecKind::BRANCH;
        c.latency = 1.0f;
        c.recip_tp = 1.0f;
        return c;

    /* --- Memoria explicita (pseudo LOAD/STORE + VM) --- */
    case MOp::LOAD:
    case MOp::LOAD_VM:
        c.kind = ExecKind::LOAD;
        c.latency = (mi.op == MOp::LOAD_VM) ? 5.0f : 4.0f;
        c.recip_tp = 0.5f; // 2 puertos de load
        return c;
    case MOp::STORE:
    case MOp::STORE_VM:
        c.kind = ExecKind::STORE;
        c.latency = 1.0f;
        c.recip_tp = 1.0f;
        return c;

    /* --- Multiplicacion / division entera --- */
    case MOp::IMUL:
        c.kind = ExecKind::MUL;
        c.latency = 3.0f;
        c.recip_tp = 1.0f;
        break;
    case MOp::IDIV:
    case MOp::DIV_U:
        c.kind = ExecKind::DIV;
        c.latency = 20.0f;  // no totalmente pipelined
        c.recip_tp = 12.0f;
        break;

    /* --- Bit ops de latencia media --- */
    case MOp::POPCNT:
    case MOp::LZCNT:
    case MOp::TZCNT:
    case MOp::BSWAP:
        c.kind = ExecKind::ALU;
        c.latency = 3.0f;
        c.recip_tp = 1.0f;
        break;

    /* --- Coma flotante --- */
    case MOp::ADDSD:
    case MOp::SUBSD:
    case MOp::MINSD:
    case MOp::MAXSD:
    case MOp::ROUNDSD:
    case MOp::UCOMISD:
    case MOp::CVTSI2SD:
    case MOp::CVTTSD2SI:
    case MOp::CVTSS2SD:
    case MOp::CVTSD2SS:
        c.kind = ExecKind::FP_ADD;
        c.latency = 4.0f;
        c.recip_tp = 0.5f;
        break;
    case MOp::MULSD:
        c.kind = ExecKind::FP_MUL;
        c.latency = 4.0f;
        c.recip_tp = 0.5f;
        break;
    case MOp::DIVSD:
    case MOp::SQRTSD:
        c.kind = ExecKind::FP_DIV;
        c.latency = 14.0f; // no pipelined
        c.recip_tp = 8.0f;
        break;
    case MOp::MOVQ_GP_XMM:
    case MOp::MOVQ_XMM_GP:
        c.kind = ExecKind::FP_ADD; // cruce de dominio GP<->FP
        c.latency = 3.0f;
        c.recip_tp = 1.0f;
        break;

    /* --- ALU entera simple + movimientos (latencia 1) --- */
    default:
        c.kind = ExecKind::ALU;
        c.latency = 1.0f;
        c.recip_tp = 1.0f;
        break;
    }

    // Si la op ademas TOCA MEMORIA (operando MEM fusionado), suma la latencia
    // del acceso: una ALU con fuente en memoria = load + op.  Un store-op
    // (dst en memoria) queda limitado por el puerto de store.
    if (touches_mem(mi)) {
        if (writes_mem(mi)) {
            c.kind = ExecKind::STORE;
            c.latency = c.latency < 1.0f ? 1.0f : c.latency;
        } else {
            c.latency += 4.0f; // load fusionado
            if (c.kind == ExecKind::ALU) c.kind = ExecKind::LOAD;
        }
    }
    return c;
}

InstrCost GenericCostModel::cost(const MInstr &mi) const {
    return generic_cost_for(mi);
}

std::unique_ptr<SchedCostModel> make_cost_model(SchedIsa isa,
                                                const std::string &cpu) {
    (void)isa;
    (void)cpu;
    // v1: solo el modelo generico (default siempre activo).  --cpu <uarch> con
    // los datos exactos de cost_x86()/cost_arm64() (mapeo MOp -> FormID) es el
    // siguiente incremento; hasta entonces cae al generico (nunca null).
    return std::unique_ptr<SchedCostModel>(new GenericCostModel());
}

} // namespace sched
} // namespace jit
