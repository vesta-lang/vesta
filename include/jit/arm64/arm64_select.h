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
 * @file arm64_select.h
 * @brief Selector de instrucciones IR -> AArch64 ( H.2, bootstrap).
 *
 * Baja una @c ir::IrFunction a TEXTO ensamblador AArch64 que Keystone ensambla a
 * bytes.  Es el primer paso del backend arm64: un template slot-por-valor (cada
 * valor SSA vive en un hueco de la pila; cada op carga operandos a scratch,
 * computa y guarda el resultado), analogo al C1 de x86.  Cubre el subconjunto
 * entero de linea recta (CONST/MOV/ADD/SUB/MUL/AND/OR/XOR/NEG/NOT/SHL/SHR/RET) +
 * la ABI AAPCS64 basica (params en x0..x7, retorno en x0).  Las ops no cubiertas
 * (ramas, floats, llamadas, memoria) marcan @c out_unsupported para caer a otra
 * ruta -- se anaden en H.2b+.
 */

#ifndef VX_JIT_ARM64_SELECT_H
#define VX_JIT_ARM64_SELECT_H

#include <string>
#include <vector>

#include "ir/ssa_ir.h"

namespace jit {
namespace arm64 {

/**
 * @brief ABI de destino.  El ISA A64 y el codegen entero son comunes; la ABI
 *        cambia por SO en variadicos, alineacion de pila y homing de args.  Para
 *        el subconjunto entero NO variadico las tres coinciden (args x0-x7,
 *        retorno x0, callee-saved x19-x28, LR=x30); el enum fija el seam para las
 *        divergencias futuras (Windows __chkstk, Apple variadicos en pila...).
 */
enum class Arm64Abi {
    AAPCS64,     ///< estandar ARM (Linux, bare-metal).
    WinArm64,    ///< Windows on ARM.
    AppleArm64,  ///< macOS/iOS Apple Silicon.
};

/**
 * @brief Emite el ensamblador AArch64 (texto) de @p fn.
 * @param fn              Funcion IR (multi-bloque + ramas + llamadas).
 * @param out_unsupported Se pone a true si aparece una op aun no soportada.
 * @param abi             ABI de destino (ver @ref Arm64Abi).  Default AAPCS64.
 * @param out_call_targets [out] Si != NULL, recibe EN ORDEN el nombre de la
 *        funcion destino de cada @c CALL (cada uno emitido como un @c bl 0
 *        placeholder).  Como el control de flujo local usa @c b/@c b.cond/@c cbnz
 *        (nunca @c bl), TODO @c bl del cuerpo es una llamada: el backend empareja
 *        el i-esimo @c bl con @c out_call_targets[i] y emite un reloc CALL26.
 * @return Texto ensamblador AArch64 (una instruccion por linea, sin etiqueta de
 *         funcion: el caller la envuelve).  Vacio si @c out_unsupported y no se
 *         pudo emitir nada util.
 */
std::string arm64_emit_asm(const ir::IrFunction &fn, bool &out_unsupported,
                           Arm64Abi abi = Arm64Abi::AAPCS64,
                           std::vector<std::string> *out_call_targets = nullptr);

} // namespace arm64
} // namespace jit

#endif // VX_JIT_ARM64_SELECT_H
