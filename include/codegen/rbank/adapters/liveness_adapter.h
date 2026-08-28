/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/adapters/liveness_adapter.h
 * @brief Adaptador de VIVACIDAD (Fase 0.25): liveness + posiciones de CALL ->
 *        @c crosses_call en un @c ValueRequirements.
 *
 * DISCIPLINA (Fase 0.25): solo TRADUCE informacion existente.  El
 * @c LivenessAdapter mira el intervalo de vida de un valor (@c
 * ir::LiveInterval, de @c compute_liveness) y las posiciones de las llamadas, y
 * rellena EXCLUSIVAMENTE @c crosses_call.  No inventa nada mas.
 *
 * SEMANTICA: un valor "cruza" una llamada si su intervalo COVERS la posicion,
 * @c covers(p) == (def <= p <= end) -- INCLUSIVO.  Asi la respuesta a "por que
 * crosses_call=true?" es SIEMPRE "el LivenessAdapter: el intervalo [def,end]
 * cubre la posicion de un call".
 *
 * NO es el mismo criterio que el resto, aunque antes lo afirmara este
 * comentario: el linear_scan (@c jit_regalloc) es estricto por la IZQUIERDA y
 * @c backend_bridge por la DERECHA.  Tres sitios, tres criterios.  Unificarlos
 * NO es libre: ver la nota sobre las dos preguntas junto a @c interval_covers.
 *
 * QUE ES UNA "LLAMADA" AQUI (decision documentada, no heuristica): las ops de
 * llamada del IR
 * (CALL/CALLIND/TAILCALL/CALLVIRT/CALLN/CALLM/CALLCLOSURE/CALLSUPER/CALLITF)
 * -- las que clobbean los caller-saved y motivan la preferencia por
 * callee-saved. Otros clobbers puntuales (DIVMOD clobbea RDX:RAX, INLINE_ASM
 * sus regs) NO son
 * @c crosses_call: son CONSTRAINTS de lane (nivel 2), un concepto distinto.  El
 * @c crosses_call alimenta una PREFERENCIA del Objective (callee-saved), no una
 * restriccion dura.
 *
 * Fase 0.25: ADITIVO, sin cambio de comportamiento (solo el test lo consume).
 */

#ifndef VESTA_CODEGEN_RBANK_ADAPTERS_LIVENESS_ADAPTER_H
#define VESTA_CODEGEN_RBANK_ADAPTERS_LIVENESS_ADAPTER_H

#include "ir/liveness.h"
#include "ir/ssa_ir.h"
#include "codegen/rbank/value_requirements.h"

#include <cstdint>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @brief True si @p op es una operacion de LLAMADA (clobbea caller-saved).
 *
 * Traduccion de la clasificacion del propio IR (no heuristica): las ops del
 * grupo de llamadas.  DIVMOD / INLINE_ASM NO estan aqui -- sus clobbers son
 * Constraints de lane, no @c crosses_call.
 *
 * @c CALLSUPER faltaba, y es la unica de la lista cuya ausencia se paga en
 * correccion: baja a un @c CALL_ABS al despachador de metodos, asi que clobbea
 * caller-saved como cualquier otra.  Un valor vivo a traves de un
 * @c super.metodo() no quedaba marcado @c crosses_call y el asignador podia
 * dejarlo en un registro volatil.  Hoy no se nota -- el @c crosses_call de
 * produccion se decide sobre MachineIR, donde @c interval.cpp si trata
 * @c CALL_ABS como posicion de llamada --, pero se notaria en cuanto este
 * adaptador gobierne el regalloc, que es para lo que existe.
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
    case ir::IrOp::CALLSUPER:
    case ir::IrOp::CALLITF: return true;
    default: return false;
    }
}

/** @brief covers(p) canonico de un intervalo: @c def <= p <= end (inclusivo).
 */
inline bool interval_covers(const ir::LiveInterval &iv, uint32_t p) noexcept {
    return p >= iv.def && p <= iv.end;
}

/*
 * OJO -- @c crosses_call responde HOY a DOS preguntas que necesitan criterios
 * DISTINTOS, y por eso NO se puede estrechar sin mas (medido 2026-08-17):
 *
 *   - PRESERVACION ("sobrevive a la llamada?"): querria el criterio ESTRICTO
 *     @c def < p < end.  Un valor DEFINIDO en @p p es el resultado de esa
 *     instruccion y no puede destruirlo quien lo produce; uno cuyo ULTIMO uso
 *     esta en @p p lo consume esa instruccion y no necesita sobrevivirla.
 *   - RAIZ DE GC (@c backend_bridge: `crosses_call && is_gc -> MEMORY`):
 * necesita "vivo EN el safepoint", que es el criterio INCLUSIVO.  Estrecharlo
 * deja de marcar raices que el stackmap necesita -- no es rendimiento, es
 * correccion.
 *
 * Cambiar este @c covers por el estricto rompe `spr`, `cfn199` y `cmb245` en la
 * e2e (919/4 -> 915/7), y esa es la razon.  El arreglo de verdad es SEPARAR las
 * dos preguntas en dos campos, no elegir un criterio para las dos.
 * Ver [[proj_bug_asm_sin_registro]] en la memoria del agente.
 */

/**
 * @brief Posiciones LINEALIZADAS de las llamadas de @p fn, en el MISMO espacio
 *        de indices que los intervalos de @p live (via @c block_start).
 *
 * Reusa la linealizacion de @c compute_liveness (bloques en orden de emision;
 * instr @c j del bloque @c b en indice @c block_start[b] + j).  Asi las
 * posiciones son consistentes con @c def/@c end -- si no lo fueran, seria una
 * incoherencia detectable (el proposito del modelo).
 */
inline std::vector<uint32_t>
collect_call_positions(const ir::IrFunction &fn,
                       const ir::LivenessResult &live) {
    std::vector<uint32_t> pos;
    for (size_t b = 0; b < fn.blocks.size() && b < live.block_start.size();
         ++b) {
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
        if (interval_covers(iv, p)) {
            crosses = true;
            break;
        }
    r.crosses_call = crosses;
    /* Aqui las dos preguntas valen LO MISMO a proposito: este adaptador no sabe
     * si el valor es un operando de asm, que es el unico caso en el que se
     * separan.  Quien lo sabe es @c backend_bridge (tiene @c reg_required). */
    r.needs_preserved = crosses;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_ADAPTERS_LIVENESS_ADAPTER_H
