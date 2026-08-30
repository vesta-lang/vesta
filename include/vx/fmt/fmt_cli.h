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
 * @file fmt_cli.h
 * @brief El subcomando `vesta fmt`.
 */

#ifndef VX_FMT_FMT_CLI_H
#define VX_FMT_FMT_CLI_H

namespace vx {
namespace fmt {
namespace cli {

/**
 * @brief Ejecuta el subcomando de formato.
 *
 * @param argc Numero de argumentos, con @p argv[0] el nombre del programa y
 *             @p argv[1] la orden (`check`, `print`) o el primer fichero.
 * @param argv Argumentos.
 * @return 0 si todo fue bien; 1 si algo fallo o, en `check`, si algun fichero
 *         no estaba formateado.
 */
int run(int argc, char **argv);

} // namespace cli
} // namespace fmt
} // namespace vx

#endif // VX_FMT_FMT_CLI_H
