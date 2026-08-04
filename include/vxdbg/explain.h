/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vxdbg/explain.h
 * @brief Explica una direccion de un binario nativo a partir del fichero
 *        acompanante de depuracion del lenguaje.
 *
 * Es el CONSUMIDOR de lo que el AOT deja en `<binario>.vxdbg`.  Existe porque
 * el binario no lleva nada que se explique a si mismo -- ni un manejador ni un
 * trap --: meterselo cambiaria el programa que despues se depura, y lo que
 * veria un depurador externo o un desensamblador ya no seria lo que se
 * compilo.  El binario muere como muere, y el informe se produce despues, con
 * los datos que quedaron aparte.
 *
 * Se apoya en el OTRO mecanismo en vez de duplicarlo: el `.symtab` que emite
 * `--debug-info=1` hace que gdb, WinDbg o un desensamblador digan `main+0x4a`,
 * y esto dice que hay ahi.
 */

#ifndef VESTA_VXDBG_EXPLAIN_H
#define VESTA_VXDBG_EXPLAIN_H

#include <string>

namespace vxdbg {

/**
 * @brief Explica un punto del codigo de un binario nativo.
 *
 * @param binario Ruta del ejecutable (se busca su `.vxdbg` al lado).
 * @param donde Punto a explicar, en la forma `funcion+0xNN` o `funcion`.
 * @param err Mensaje de error si no se pudo.
 * @return true si se produjo el informe.
 */
bool explain_location(const std::string &binario, const std::string &donde,
                      std::string &err);

} // namespace vxdbg

#endif // VESTA_VXDBG_EXPLAIN_H
