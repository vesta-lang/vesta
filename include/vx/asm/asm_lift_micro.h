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

#include "vx/asm/asm_lift_reason.h" // en que se atasco el elevado
#include "vx/asm/instr_db.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ir {
struct IrFunction;
using IrValueId = uint32_t; // == ir/ssa_ir.h (typedef, no se puede fwd-declarar)
} // namespace ir

namespace vx {

/**
 * @brief Lifta el bloque @p body (asm de la ISA @p isa) a una secuencia de
 *        @c IrOp::ASM_MICRO en @p block, UNA por instruccion, SI y solo si TODAS
 *        sus instrucciones son formas conocidas por la DB y sus operandos son
 *        O BIEN inexistentes (barreras / nop / pause), O BIEN registros de
 *        FiSICO FIJO (: popcnt rax, rbx).  Es transaccional: valida el
 *        bloque entero antes de emitir nada.
 *
 * @param slot_of mapa REGISTRO-CANoNICO -> slot ALLOCA (SSA) de las variables
 *        Vesta ligadas via @c register() en el scope actual.  Un operando que
 *        usa uno de estos NO es fisico opaco: lleva su valor SSA (@c value) y
 *        su registro fisico fijo (@c fixed_phys), de modo que el asignador de
 *        registros respeta el PIN (constraint en el RA, no en el ensamblador)
 *        y su intervalo cubre el asm.  El resto (no ligados) son fisico fijo
 *        SIN valor SSA.
 *
 * @return true si el bloque se lifto por completo (0 INLINE_ASM); false si
 *         aparece una instruccion desconocida o un operando no soportado
 *         (MEM/IMM/FP/VEC/implicito) -> el llamador emite @c INLINE_ASM.
 */
/**
 * @param bloque_salida Si no es nulo, recibe el bloque del IR por el que SIGUE
 *        la ejecucion despues del asm.
 *
 * Hoy es siempre @p block, porque un bloque sin saltos empieza y acaba en el
 * mismo sitio.  Existe porque deja de serlo en cuanto el asm lleve flujo de
 * control: entonces el elevado crea bloques y ramas, y la ejecucion continua en
 * OTRO.  Sin esta salida, el codigo que va detras del `asm` se colgaria del
 * bloque de entrada -- que ya no es donde termina -- y acabaria en una rama que
 * no se ejecuta.  Es la unica pieza del contrato que hay que cambiar antes de
 * poder elevar un salto, y cambiarla ahora, con el comportamiento intacto, deja
 * el paso siguiente sin tocar a los llamantes.
 */
bool asm_lift_micro(
    ir::IrFunction &fn, uint32_t block, instr_db::Isa isa,
    const std::string &body, uint32_t line,
    const std::unordered_map<std::string, ir::IrValueId> &slot_of = {},
    AsmMotivoOpaco *motivo = nullptr, uint32_t *bloque_salida = nullptr);

/**
 * @brief Instrucciones que la base de datos no supo resolver en lo que va de
 *        proceso, como pares (mnemonico, motivo).
 *
 * Que una no este no impide compilar -- se trata como caja opaca -- pero casi
 * siempre significa que falta en la base de datos, asi que conviene poder
 * verlas en vez de que se pierdan en silencio.  Con @c VESTA_ASM_DB_GAPS=1 se
 * avisa ademas de cada una la primera vez que aparece.
 *
 * @return La lista, ordenada y sin repetidos.
 */
std::vector<std::pair<std::string, std::string>> asm_db_huecos();

} // namespace vx

#endif // VESTA_VX_ASM_ASM_LIFT_MICRO_H
