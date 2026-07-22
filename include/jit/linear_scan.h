/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/linear_scan.h
 * @brief Register allocator linear-scan para el JIT ( D.7, commit 3).
 *        Ver doc/REGALLOC.md.
 *
 * Asigna a cada registro virtual un registro fisico O un spill slot de stack,
 * a partir de los live intervals (interval.h) y el descriptor del target
 * (target_reginfo.h).  Generico: NO usa literales @c MReg::*; lee los pools
 * de registros del @c TargetRegInfo.
 *
 * = Algoritmo (v1) =
 *
 * Linear-scan estilo Poletto-Sarkar, asignacion all-or-nothing por intervalo
 * (sin splitting a mitad de intervalo todavia; se añade en una fase
 * posterior persiguiendo el ultimo %).  Por CLASE de registro (GP/FP) de
 * forma independiente:
 *
 *   1. Ordenar intervalos por inicio.
 *   2. Mantener una lista @c active (intervalos con reg asignado, ordenada
 *      por fin).  Al avanzar, EXPIRAR los que ya terminaron (liberan su reg).
 *   3. Para cada intervalo: si cruza un CALL, solo puede usar registros
 *      callee-saved (los caller-saved se destruyen en la llamada) -> esto
 *      empuja los valores live-across-call a R12-R15 o a spill, sin necesidad
 *      de fixed intervals explicitos.
 *   4. Si no hay registro usable libre: SPILL.  Victima = el intervalo (entre
 *      el actual y los activos robables) con el FIN mas lejano (Poletto).  El
 *      perdedor va a un spill slot para toda su vida.
 *
 * = Salida =
 *
 * Por vreg: un fisico (@c REG) o un spill slot (@c SPILL), o @c NONE si el
 * vreg esta muerto.  Mas la lista de callee-saved realmente usados (push/pop
 * en prologue/epilogue) y el numero de spill slots (tamano extra del frame).
 * El rewrite (commit 4) consume esto para reescribir los operandos VREG.
 */

#ifndef VESTA_JIT_LINEAR_SCAN_H
#define VESTA_JIT_LINEAR_SCAN_H

#include "codegen/regalloc.h"   // codegen::RegAlloc (estructura general del resultado)
#include "jit/interval.h"
#include "jit/target_reginfo.h"

#include <cstdint>
#include <vector>

namespace jit {

/**
 * @brief Ejecuta el linear-scan sobre los intervals de una funcion.
 *
 * @param ivs  Live intervals + posiciones de CALL (de @c build_intervals).
 * @param tri  Descriptor del target (pools de registros por clase).
 * @return     Asignacion vreg -> reg/slot (@c codegen::RegAlloc, estructura general).
 */
codegen::RegAlloc linear_scan(const IntervalResult &ivs, const TargetRegInfo &tri);

} // namespace jit

#endif // VESTA_JIT_LINEAR_SCAN_H
