/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/effects/effects_report.h
 * @brief Proyeccion del modelo de efectos a texto legible (--analyze).
 *        Muestra, por funcion, los EFECTOS inferidos y los CONTRATOS derivados,
 *        mas un reporte de LAGUNAS de precision.  Es una PROYECCION del mismo
 *        SemanticSummary que consume el compilador (invariante: no re-lee el IR).
 */
#ifndef ANALYSIS_EFFECTS_EFFECTS_REPORT_H
#define ANALYSIS_EFFECTS_EFFECTS_REPORT_H

#include "analysis/effects/ir_effects.h" // Backend: el informe dice para quien habla

#include <ostream>

namespace ir {
struct IrModule;
}

namespace analysis {
namespace effects {

/// Imprime el reporte de efectos + contratos + lagunas del modulo @p mod.
/// @p backend dice para quien se describe el programa: una misma op baja
/// distinto en cada uno, y varias solo existen porque hay un runtime detras.
void print_effects_report(std::ostream &os, const ir::IrModule &mod,
                          Backend backend = Backend::Vm);

/// Emite el MISMO modelo (efectos local/cierre + contratos derivados +
/// estructura + lagunas) como un objeto JSON autocontenido para los diagramas
/// / LSP: `{"functions":[...],"gaps":{...}}`.  Es la proyeccion JSON del
/// SemanticSummary -- misma fuente que --analyze y que consume el compilador,
/// para que diagramas, --analyze y codegen no puedan divergir.
void effects_json(std::ostream &os, const ir::IrModule &mod,
                  Backend backend = Backend::Vm);

} // namespace effects
} // namespace analysis

#endif // ANALYSIS_EFFECTS_EFFECTS_REPORT_H
