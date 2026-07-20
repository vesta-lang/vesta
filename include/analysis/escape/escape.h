/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/escape/escape.h
 * @brief EscapeAnalysis: responde "¿escapa esta raiz (ALLOCA local o parametro)
 *        al exterior de la funcion?".  Analisis de PRIMERA CLASE sobre la base
 *        de hechos compartida (IRFacts + PointsTo) -- aporta lo que ningun otro:
 *        distinguir memoria observable-por-el-caller de scratch LOCAL.
 *
 * COMPLETO, no conservador: cada op se MODELA (posicion de DIRECCION = no
 * captura; DERIVACION = transitiva a la misma raiz; COMPARACION = lee el valor,
 * no captura; el resto = CAPTURA -> escapa).  No hay bail "op desconocida ->
 * escapa todo".  Los CALL se resuelven INTERPROCEDURALMENTE: un arg escapa solo
 * si el callee captura ESE parametro (summary escaping_params, cerrado por
 * punto-fijo del callgraph); un callee externo/dinamico captura todos sus
 * params (respuesta CORRECTA sin su cuerpo, no un bail).
 *
 * Doble faceta por funcion:
 *  - escaping_stack: ALLOCA locales cuya DIRECCION escapa (sus escrituras SI son
 *    observables por el caller; las de las que NO escapan son scratch local).
 *  - escaping_params: indices de parametro que la funcion deja escapar (para que
 *    el caller resuelva si su ALLOCA/param escapa al pasarlo aqui).
 *
 * Consumidor: EffectAnalysis filtra del efecto OBSERVABLE las lecturas/
 * escrituras a Stack roots NO escapantes -> una reduccion que escribe solo
 * Stack#acc_slot pasa a readonly para el caller.
 */
#ifndef ANALYSIS_ESCAPE_ESCAPE_H
#define ANALYSIS_ESCAPE_ESCAPE_H

#include "analysis/facts/ir_facts.h"
#include "analysis/memory/points_to.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ir {
struct IrFunction;
struct IrModule;
} // namespace ir

namespace analysis {

/// Escape de UNA funcion: allocas locales que escapan + parametros que escapan.
struct EscapeInfo {
    std::unordered_set<uint32_t> escaping_stack;  ///< value-id del ALLOCA que escapa
    std::unordered_set<int32_t>  escaping_params; ///< indices de param que escapan
    bool stack_escapes(uint32_t root) const {
        return escaping_stack.count(root) > 0;
    }
};

/// Oraculo interproc: ¿el callee @p callee_name captura/deja escapar su
/// parametro @p param_idx?  Lo provee el punto-fijo del modulo; para un callee
/// desconocido devuelve true (captura todo -- respuesta correcta sin su cuerpo).
using CalleeEscapesParam =
    std::function<bool(const std::string &callee_name, int32_t param_idx)>;

/// Computa la EscapeAnalysis LOCAL de @p fn dado el oraculo interproc.  CONSUME
/// la base de hechos (facts + pt), no la construye.  O(instrucciones x operandos).
EscapeInfo compute_escape(const ir::IrFunction &fn, const IrFacts &facts,
                          const PointsTo &pt, const CalleeEscapesParam &callee);

/// EscapeAnalysis de TODO UN MoDULO: cierra escaping_params por punto-fijo del
/// callgraph (un param escapa si se usa en posicion de captura O se pasa a un
/// callee que captura su param) y devuelve el EscapeInfo COMPLETO de cada
/// funcion (con escaping_stack ya resuelto interproceduralmente).  Recibe la
/// base de hechos por funcion via las factories (no la construye aqui).
std::unordered_map<std::string, EscapeInfo> compute_escape_module(
    const ir::IrModule &mod,
    const std::function<const IrFacts &(const ir::IrFunction &)> &facts_of,
    const std::function<const PointsTo &(const ir::IrFunction &)> &pt_of);

/// Marcador de analisis para el AnalysisManager (cachea EscapeInfo por funcion).
struct EscapeAnalysis {
    using Result = EscapeInfo;
    static char ID;
};

} // namespace analysis

#endif // ANALYSIS_ESCAPE_ESCAPE_H
