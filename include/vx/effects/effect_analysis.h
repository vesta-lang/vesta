/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/effects/effect_analysis.h
 * @brief Gestor de analisis de efectos: DOS niveles.  (a) efecto LOCAL por nodo
 *        IR, barato y CACHEADO, invalidado cuando el nodo muta; (b) resumen por
 *        FUNCION, resultado de un pase de punto-fijo sobre el callgraph.  Los
 *        optimizadores y el tooling preguntan aqui; NUNCA re-leen el IR ni
 *        computan efectos por su cuenta (ese es el invariante que garantiza que
 *        `--analyze`, diagramas y LSP muestren lo mismo que consume el compilador).
 *
 * Invalidacion (especificada desde el inicio para evitar bugs de staleness):
 *   mutacion del IR de un nodo
 *     -> invalidate_node(node)         (borra el cache local del nodo)
 *     -> invalidate_function(fn)       (el summary de la funcion queda sucio)
 *     -> invalidate transitiva por el callgraph (los callers dentro de la SCC y
 *        aguas arriba tambien quedan sucios)
 *   El recomputo es perezoso: summary(fn) recomputa si esta sucio.
 *
 * Fase 0: esqueleto (interfaz + cache local + invalidacion).  La logica de
 * `local(node)` (mapa IrOp -> SemanticEffects) entra en Fase 1; el punto-fijo de
 * `summary(fn)` en Fase 2.
 */
#ifndef VX_EFFECTS_EFFECT_ANALYSIS_H
#define VX_EFFECTS_EFFECT_ANALYSIS_H

#include "vx/effects/effects.h"
#include "vx/effects/ir_effects.h"
#include "vx/effects/summary.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ir {
struct IrModule;
struct IrFunction;
struct IrInstr;
} // namespace ir

namespace vx {
namespace fx {

/**
 * @brief Gestor de analisis de efectos de un modulo IR.  Mantiene el cache
 *        local por nodo y los summaries por funcion, con invalidacion.
 */
class EffectAnalysis {
public:
    EffectAnalysis() = default;

    /// Efecto LOCAL de un nodo IR (intrinseco al opcode + operandos).  Cacheado.
    /// Fase 1 rellena el mapeo real; Fase 0 devuelve el neutro Complete.
    EffectAnalysisResult local(const ir::IrFunction &fn, const ir::IrInstr &ins);

    /// Resumen COMPLETO de una funcion (efectos local/closure + estructura +
    /// interproc).  Recomputa por punto-fijo si esta sucio.  Fase 2 rellena la
    /// logica; Fase 0 devuelve un summary vacio Complete.
    const FunctionSummary &summary(const ir::IrModule &mod,
                                   const ir::IrFunction &fn);

    /// Resumen del modulo entero (mapa symbol -> summary).
    const ModuleSummary &module_summary(const ir::IrModule &mod);

    /// Lagunas de precision acumuladas (que ops faltan por modelar, donde la
    /// opacidad es fundamental).  Se puebla al construir los summaries.
    const EffectGaps &gaps() const { return gaps_; }

    // ---- Invalidacion ----
    /// Un nodo muto: borra su cache local + marca su funcion sucia.
    void invalidate_node(const ir::IrFunction &fn, const ir::IrInstr &ins);
    /// Una funcion muto: su summary + el de sus callers transitivos quedan sucios.
    void invalidate_function(const std::string &fn_name);
    /// Borra TODO (reconstruccion completa).
    void clear();

private:
    // Clave de nodo: (funcion, indice-de-instruccion estable).  En Fase 0 usamos
    // la direccion del IrInstr como clave provisional; Fase 1 fija una clave
    // estable (id de instruccion) cuando el IR lo exponga.
    std::unordered_map<const void *, EffectAnalysisResult> local_cache_;
    std::unordered_map<std::string, FunctionSummary>       summary_cache_;
    std::unordered_map<std::string, bool>                  dirty_; // fn -> sucio
    ModuleSummary                                          module_cache_;
    EffectGaps                                             gaps_;
    bool module_dirty_ = true;

    FunctionSummary compute_summary(const ir::IrModule &mod,
                                    const ir::IrFunction &fn);
};

} // namespace fx
} // namespace vx

#endif // VX_EFFECTS_EFFECT_ANALYSIS_H
