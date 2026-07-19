/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/effects/effects_report.h
 * @brief Proyeccion del modelo de efectos a texto legible (--analyze --effects).
 *        Muestra, por funcion, los EFECTOS inferidos y los CONTRATOS derivados,
 *        mas un reporte de LAGUNAS de precision.  Es una PROYECCION del mismo
 *        SemanticSummary que consume el compilador (invariante: no re-lee el IR).
 */
#ifndef VX_EFFECTS_EFFECTS_REPORT_H
#define VX_EFFECTS_EFFECTS_REPORT_H

#include <ostream>

namespace ir {
struct IrModule;
}

namespace vx {

/// Imprime el reporte de efectos + contratos + lagunas del modulo @p mod.
void print_effects_report(std::ostream &os, const ir::IrModule &mod);

} // namespace vx

#endif // VX_EFFECTS_EFFECTS_REPORT_H
