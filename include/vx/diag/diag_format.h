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
 * @file diag_format.h
 * @brief Renderizado de diagnosticos en formato humano o MaQUINA (JSON/SARIF).
 *
 * Como cada @c Diagnostic lleva codigo + args + localizacion (datos), se puede
 * volcar a un formato estandar de herramientas sin perder informacion:
 *   - @c Text  : formato gcc-like legible (el de siempre).
 *   - @c Json  : array JSON con codigo, severidad, ubicacion, args y mensaje
 *                formateado en el idioma activo.  Para IDEs/CI propios.
 *   - @c Sarif : SARIF 2.1.0 (el estandar que consumen GitHub, VS Code y la
 *                mayoria de CIs) -- @c runs[].results[] con @c ruleId = codigo.
 */

#ifndef VX_DIAG_FORMAT_H
#define VX_DIAG_FORMAT_H

#include <ostream>
#include <string>

#include "vx/diagnostic.h"

namespace vx {

/// Formato de salida de los diagnosticos.
enum class DiagFormat { Text, Json, Sarif };

/// Convierte @p s ("text"|"json"|"sarif") a @ref DiagFormat.  Desconocido ->
/// Text.  @p ok (opcional) recibe si el nombre se reconocio.
DiagFormat parse_diag_format(const std::string &s, bool *ok = nullptr);

/// Renderiza @p diags en @p os con el formato @p fmt (en el idioma activo para
/// los mensajes).
void render_diagnostics(std::ostream &os, const Diagnostics &diags,
                        DiagFormat fmt);

} // namespace vx

#endif // VX_DIAG_FORMAT_H
