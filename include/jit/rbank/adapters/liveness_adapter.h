/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/rbank/adapters/liveness_adapter.h
 * @brief Adaptador de VIVACIDAD (Fase 0.25): liveness + posiciones de CALL ->
 *        @c crosses_call en un @c ValueRequirements.
 *
 * DISCIPLINA (Fase 0.25): solo TRADUCE informacion existente.  El
 * @c LivenessAdapter mira el intervalo de vida de un valor (@c ir::LiveInterval,
 * de @c compute_liveness) y las posiciones de las llamadas, y rellena
 * EXCLUSIVAMENTE @c crosses_call.  No inventa nada mas.
 *
 * SEMANTICA canonica (reusada del linear_scan, NO redefinida): un valor "cruza"
 * una llamada si su intervalo COVERS la posicion de la llamada, donde
 * @c covers(p) == (def <= p <= end) -- mismo criterio inclusivo que el allocator
 * de produccion.  Asi la respuesta a "¿por que crosses_call=true?" es SIEMPRE
 * "el LivenessAdapter: el intervalo [def,end] cubre la posicion de un call".
 *
 * QUE ES UNA "LLAMADA" AQUI (decision documentada, no heuristica): las ops de
 * llamada del IR (CALL/CALLIND/TAILCALL/CALLVIRT/CALLN/CALLM/CALLCLOSURE/CALLITF)
 * -- las que clobbean los caller-saved y motivan la preferencia por callee-saved.
 * Otros clobbers puntuales (DIVMOD clobbea RDX:RAX, INLINE_ASM sus regs) NO son
 * @c crosses_call: son CONSTRAINTS de lane (nivel 2), un concepto distinto.  El
 * @c crosses_call alimenta una PREFERENCIA del Objective (callee-saved), no una
 * restriccion dura.
 *
 * Fase 0.25: ADITIVO, sin cambio de comportamiento (solo el test lo consume).
 */

#ifndef VESTA_JIT_RBANK_ADAPTERS_LIVENESS_ADAPTER_H
#define VESTA_JIT_RBANK_ADAPTERS_LIVENESS_ADAPTER_H

#include "ir/liveness.h"
#include "ir/ssa_ir.h"
#include "jit/rbank/value_requirements.h"

#include <cstdint>
#include <vector>

namespace jit {
namespace rbank {

/**
 * @brief True si @p op es una operacion de LLAMADA (clobbea caller-saved).
 *
 * Traduccion de la clasificacion del propio IR (no heuristica): las ops del
 * grupo de llamadas.  DIVMOD / INLINE_ASM NO estan aqui -- sus clobbers son
 * Constraints de lane, no @c crosses_call.
 */
inline bool ir_op_is_call(ir::IrOp op) noexcept {
    switch (op) {
    case ir::IrOp::CALL:
    case ir::IrOp::CALLIND:
    case ir::IrOp::TAILCALL:
    case ir::IrOp::CALLVIRT:
    case ir::IrOp::CALLN:
    case ir::IrOp::CALLM:
    case ir::IrOp::CALLCLOSURE:
    case ir::IrOp::CALLITF:
        return true;
    default:
        return false;
    }
}

/** @brief covers(p) canonico de un intervalo: @c def <= p <= end (inclusivo). */
inline bool interval_covers(const ir::LiveInterval &iv, uint32_t p) noexcept {
    return p >= iv.def && p <= iv.end;
}

/**
 * @brief Posiciones LINEALIZADAS de las llamadas de @p fn, en el MISMO espacio
 *        de indices que los intervalos de @p live (via @c block_start).
 *
 * Reusa la linealizacion de @c compute_liveness (bloques en orden de emision;
 * instr @c j del bloque @c b en indice @c block_start[b] + j).  Asi las
 * posiciones son consistentes con @c def/@c end -- si no lo fueran, seria una
 * incoherencia detectable (el proposito del modelo).
 */
inline std::vector<uint32_t> collect_call_positions(const ir::IrFunction &fn,
                                                    const ir::LivenessResult &live) {
    std::vector<uint32_t> pos;
    for (size_t b = 0; b < fn.blocks.size() && b < live.block_start.size(); ++b) {
        const uint32_t base = live.block_start[b];
        const auto &instrs = fn.blocks[b].instrs;
        for (size_t j = 0; j < instrs.size(); ++j)
            if (ir_op_is_call(instrs[j].op))
                pos.push_back(base + static_cast<uint32_t>(j));
    }
    return pos;
}

/**
 * @brief Rellena EXCLUSIVAMENTE @c crosses_call de @p r desde el intervalo
 *        @p iv y las posiciones de llamada @p call_positions.
 */
inline void populate_liveness_requirements(
    ValueRequirements &r, const ir::LiveInterval &iv,
    const std::vector<uint32_t> &call_positions) noexcept {
    bool crosses = false;
    for (uint32_t p : call_positions)
        if (interval_covers(iv, p)) { crosses = true; break; }
    r.crosses_call = crosses;
}

} // namespace rbank
} // namespace jit

#endif // VESTA_JIT_RBANK_ADAPTERS_LIVENESS_ADAPTER_H
