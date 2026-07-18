/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file jit/sched/machine_sched.h
 * @brief Scheduler machine-level (list scheduling, C2.15): reordena las MInstr
 *        de cada bloque para ocultar latencias (loads/mul/div/fp) y exponer
 *        paralelismo a nivel de instruccion (ILP) al core superescalar/OoO.
 *
 * Correccion primero: solo reordena instrucciones INDEPENDIENTES.  El DAG de
 * dependencias se construye con los efectos COMPLETOS de @ref machine_effects
 * (registros explicitos + implicitos, flags, orden de memoria, barreras).  El
 * orden se decide con el @ref SchedCostModel (latencia/throughput/puertos).
 *
 * Se aplica POR BLOQUE; las barreras (call/ret/salto/safepoint/asm) parten el
 * bloque en regiones y nunca se cruzan.  No cambia la semantica: reordenar
 * instrucciones sin dependencias produce el mismo resultado observable.
 */

#ifndef VESTA_JIT_SCHED_MACHINE_SCHED_H
#define VESTA_JIT_SCHED_MACHINE_SCHED_H

#include "jit/machine_ir.h"
#include "jit/sched/cost_model.h"
#include "jit/sched/machine_effects.h"

namespace jit {
namespace sched {

/**
 * @brief Reordena en sitio las MInstr de cada bloque de @p mf para maximizar
 *        el ILP, respetando todas las dependencias y barreras.
 *
 * @param mf    funcion maquina (se modifica en sitio).
 * @param cm    modelo de coste (latencia/puertos) -- p.ej. GenericCostModel.
 * @param isa   ISA para leer los efectos de la DB correcta.
 * @return numero de instrucciones que cambiaron de posicion (0 = sin cambios).
 */
int schedule_function(MFunction &mf, const SchedCostModel &cm,
                      EffIsa isa = EffIsa::X86);

} // namespace sched
} // namespace jit

#endif // VESTA_JIT_SCHED_MACHINE_SCHED_H
