/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vxdbg/store_cli.h
 * @brief El subcomando `vm vxdbg`: mirar el almacen de diagnostico.
 *
 * POR QUE UN SUBCOMANDO Y NO UNA BANDERA.  Esto no modifica una compilacion:
 * opera sobre el almacen, con sus propias ordenes.  Es el mismo caso que
 * `vm pkg`, y por eso sigue su forma.
 *
 * QUE HACE FALTA SABER ANTES DE USARLO.  El almacen crece con el uso: cada
 * compilacion publica una raiz y escribe un paquete, y hasta hoy nada retiraba
 * ninguna de las dos cosas.  `status` dice cuanto hay y cuanto sobra; no borra.
 */
#ifndef VXDBG_STORE_CLI_H
#define VXDBG_STORE_CLI_H

namespace vxdbg {
namespace cli {

/**
 * @brief Atiende `vm vxdbg <orden> [...]`.
 * @param argc Argumentos, ya sin el literal `vxdbg`.
 * @param argv Idem.
 * @return Codigo de salida del proceso.
 */
int run(int argc, char **argv);

} // namespace cli
} // namespace vxdbg

#endif // VXDBG_STORE_CLI_H
