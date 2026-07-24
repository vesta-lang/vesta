/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file regalloc.h
 * @brief Asignador de registros VM por barrido lineal (linear scan).
 *
 * Asigna los 16 registros VM (r0-r15) a los valores SSA de una funcion.
 *
 * Convencion de llamada pre-asignada:
 *   - param[0] -> r1, param[1] -> r2, ..., param[11] -> r12
 *   - r0: valor de retorno (se asigna antes del ret)
 *   - r13: scratch SECUNDARIO del emisor  (NO asignado -- @c SCRATCH2_REG)
 *   - r14: scratch PRIMARIO del emisor    (NO asignado -- @c SCRATCH_REG)
 *   - r15: argc para llamadas             (NO asignado -- @c ARGC_REG)
 *
 * Es decir, el pool asignable es r0-r12 (@c ALLOC_REGS = 13).
 *
 * (Esta cabecera afirmaba antes que "r0-r13 estan disponibles" y que "r13:
 * disponible para el asignador".  Era FALSO desde que r13 paso a ser el
 * scratch secundario: tres afirmaciones -- las dos del texto y las constantes
 * -- se contradecian entre si.  Corregido.)
 *
 * POR QUE HAY DOS SCRATCH.  El emisor necesita registros propios para
 * materializar valores DERRAMADOS al construir una instruccion: @c load_src usa
 * r14 para el primer operando y r13 para el segundo.  Con instrucciones de TRES
 * operandos derramados esos dos no bastan y hay que pasar por la pila (ver
 * @c emit_three_reg_op en ir_emitter.cpp) -- limitacion conocida, no descuido.
 *
 * PENDIENTE (reserva DEMAND-DRIVEN).  Reservar los tres SIEMPRE cuesta 3 de 16
 * registros incluso en funciones que no los necesitan: r15 solo hace falta si
 * hay llamadas, y r13 solo si hay derrames.  El proyecto ya tiene el precedente
 * de reserva por demanda en el path vreg (@c fn_needs_vec_reserve libera
 * XMM10-13 en funciones sin ops vectoriales).  Aplicarlo aqui exige una pasada
 * optimista (asignar con el pool ampliado y repetir si aparece un derrame),
 * porque "hay derrames" depende de cuantos registros haya.
 *
 * Cuando el numero de valores vivos simultaneamente supera ALLOC_REGS,
 * el asignador derrama (spill) los valores menos prioritarios a slots
 * de pila. El emisor es responsable de emitir los push/pop de spill.
 */

#ifndef REGALLOC_H
#define REGALLOC_H

#include "codegen/regalloc.h"
#include "ir/liveness.h"
#include <cstdint>
#include <vector>

namespace ir {

/** @brief Numero de registros VM disponibles para asignacion (r0-r12). */
static constexpr int ALLOC_REGS = 13;

/** @brief Registro scratch primario del emisor (no asignado por el allocator).
 */
static constexpr int SCRATCH_REG = 14; // r14

/** @brief Registro scratch secundario para derrames de segundo operando. */
static constexpr int SCRATCH2_REG = 13; // r13 (reservado junto con r14)

/** @brief Registro reservado para argc en llamadas. */
static constexpr int ARGC_REG = 15; // r15

/**
 * @brief Obtiene el nombre de texto de un registro VM (p.ej. "r0", "r14").
 * @param reg Numero de registro (0-15).
 * @return Cadena del registro.
 */
const char *reg_name(int reg);

} // namespace ir

#endif // REGALLOC_H
