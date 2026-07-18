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
 * @file vx/asm/asm_lift_micro.h
 * @brief Lift de instrucciones asm OPACAS (sin op IR tipada equivalente) a
 *        @c IrOp::ASM_MICRO: cada instruccion pasa a ser IR llevando su
 *        identidad en la base de datos (isa + form_id) de donde se consultan sus
 *        efectos.  Cubre el subconjunto SIN operandos de registro
 *        (mfence/lfence/sfence/pause/nop...): su unico efecto observable es una
 *        barrera de memoria o ninguno, asi que no necesita enhebrar registros ni
 *        fijarlos en el asignador.
 *
 * El caso con operandos de registro (SIMD, cpuid...) necesita substitucion de
 * placeholders + pinning en el regalloc y llega en un incremento posterior; el
 * lifter general instruccion-a-instruccion (@ref asm_lift_general) reutilizara
 * este mecanismo para lo no computacional.  Crece bajo demanda: instruccion
 * desconocida por la DB o con operandos -> devuelve false (el llamador emite la
 * caja opaca @c INLINE_ASM).
 */

#ifndef VESTA_VX_ASM_ASM_LIFT_MICRO_H
#define VESTA_VX_ASM_ASM_LIFT_MICRO_H

#include "vx/asm/instr_db.h"

#include <cstdint>
#include <string>

namespace ir {
struct IrFunction;
} // namespace ir

namespace vx {

/**
 * @brief Lifta el bloque @p body (asm de la ISA @p isa) a una secuencia de
 *        @c IrOp::ASM_MICRO en @p block, UNA por instruccion, SI y solo si TODAS
 *        sus instrucciones son formas conocidas por la DB y SIN operandos de
 *        registro (barreras / nop / pause...).  Es transaccional: valida el
 *        bloque entero antes de emitir nada.
 *
 * @return true si el bloque se lifto por completo (0 INLINE_ASM); false si
 *         aparece una instruccion desconocida o con operandos -> el llamador
 *         emite @c INLINE_ASM.
 */
bool asm_lift_micro(ir::IrFunction &fn, uint32_t block, instr_db::Isa isa,
                    const std::string &body, uint32_t line);

} // namespace vx

#endif // VESTA_VX_ASM_ASM_LIFT_MICRO_H
