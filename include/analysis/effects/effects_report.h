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

#include <ostream>

namespace ir {
struct IrModule;
}

namespace analysis {
namespace effects {

/// Imprime el reporte de efectos + contratos + lagunas del modulo @p mod.
void print_effects_report(std::ostream &os, const ir::IrModule &mod);

} // namespace effects
} // namespace analysis

#endif // ANALYSIS_EFFECTS_EFFECTS_REPORT_H
