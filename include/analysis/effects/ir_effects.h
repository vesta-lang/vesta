/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/effects/ir_effects.h
 * @brief Motor IR -> SemanticEffects.  Es el UNICO motor de efectos del caso
 *        principal: el asm lifteado ya es IR normal y pasa por aqui como
 *        cualquier otra instruccion.  Da (a) el efecto local de una instruccion
 *        (con la memoria clasificada a AbstractLoc via un mini points-to sobre
 *        los def-use de la funcion), y (b) el agregado local de una funcion
 *        (fold seq dentro de bloque, join entre bloques).
 */
#ifndef ANALYSIS_EFFECTS_IR_EFFECTS_H
#define ANALYSIS_EFFECTS_IR_EFFECTS_H

#include "analysis/facts/ir_facts.h"
#include "analysis/effects/effects.h"

#include <cstdint>
#include <map>
#include <vector>

namespace ir {
struct IrFunction;
struct IrInstr;
using IrValueId = uint32_t; // igual que la definicion en ssa_ir.h
} // namespace ir

namespace analysis {
namespace effects {

/// Clasifica el puntero @p ptr (value id) a una localizacion abstracta a partir
/// de los HECHOS (def-use) de la funcion: ALLOCA->Stack, alloc->Heap,
/// static->Global, parametro->ArgDerived, GEP/cast->recurse.  Unknown si no se
/// puede.  Consume @c analysis::IrFacts (NO reconstruye el def-use).
AbstractLoc classify_ptr(const ir::IrFunction &fn, const analysis::IrFacts &facts,
                         ir::IrValueId ptr);

/// Registro de LAGUNAS de precision: hace VISIBLE donde el motor tuvo que subir
/// al efecto TOP y por que.  Permite reportar (a) que IrOps faltan por modelar
/// (cobertura -> mejorar el motor) y (b) donde la opacidad es fundamental
/// (FFI/dinamico -> oportunidades de optimizacion que solo un cambio de codigo
/// del usuario desbloquea).  Sin esto, un top() a secas ocultaria ambos.
struct EffectGaps {
    std::map<int, uint32_t>           unmodeled_ops;  ///< IrOp (int) -> nº de veces.
    std::map<UnknownReason, uint32_t> by_reason;      ///< motivo -> nº de veces.
    uint32_t total_top = 0;                           ///< sitios que subieron a top.

    void record(int op, UnknownReason why) {
        ++total_top;
        ++by_reason[why];
        if (reason_is_gap(why)) ++unmodeled_ops[op];
    }
    bool empty() const { return total_top == 0; }
};

/// Efecto LOCAL de UNA instruccion IR (con completeness + motivo).  El asm
/// lifteado no es especial: llega como ADD/LOAD/STORE/... normales.  INLINE_ASM/
/// ASM_MICRO (residuo opaco) se analizan aparte con tags.
EffectAnalysisResult effects_of_instr(const ir::IrFunction &fn,
                                      const analysis::IrFacts &facts,
                                      const ir::IrInstr &ins);

/// Efecto LOCAL agregado de una funcion completa (fold de sus bloques).  Es el
/// `.local` del SemanticSummary; el `.closure` (interproc) lo anade la Fase 2.
/// @p gaps (si != null) acumula las lagunas de precision encontradas.
EffectAnalysisResult function_local_effects(const ir::IrFunction &fn,
                                            EffectGaps *gaps = nullptr);

} // namespace effects
} // namespace analysis

#endif // ANALYSIS_EFFECTS_IR_EFFECTS_H
