/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/naked_native.h
 * @brief Bug/feature 198: soporte de inline-asm @Naked que referencia SIMBOLOS
 *        PROPIOS (funciones + globales del modulo) en interp (`-m vm`) y JIT
 *        (`-m jit`).
 *
 * Una funcion @Naked con cuerpo asm puro (p.ej. `call add\nret`) tiene
 * semantica NATIVA: el asm es el cuerpo (sin prologo/epilogo VM), los
 * argumentos llegan en los registros del ABI nativo del host (rdi/rsi... SysV
 * o rcx/rdx... Win64) y el retorno sale en rax; su propio `ret` cierra la
 * funcion.  El interprete no puede representar eso en bytecode VM (por eso
 * @c IrFunction::is_naked documenta "el interprete lo ignora").
 *
 * La solucion aqui es una mini-AOT AL VUELO: cuando el codigo VM/JIT (VM_ABI)
 * llama a una funcion @Naked, la llamada se ha bajado a un CALLN al dispatcher
 * @c vrt_naked_dispatch.  El dispatcher (1) native-compila la funcion @Naked y
 * TODAS sus callees Vex alcanzables via @c vreg_compile_native (ABI HOST_LEAF,
 * naked), (2) resuelve sus relocs a direcciones VIVAS (funcion -> code cache;
 * global -> direccion HOST del slot en @c vm_mem, que es donde el codigo VM_ABI
 * escribe/lee el global), (3) cachea el resultado por nombre, y (4) invoca la
 * entrada nativa con los argumentos ya marshalizados desde los registros VM
 * (R2..) al ABI nativo -- exactamente el mismo puente que @c CALLN ya usa para
 * FFI.  Retorno de la nativa (rax) -> R0.
 *
 * Cero coste si el programa no usa @Naked con simbolos: el dispatcher solo se
 * registra (como cualquier virtual-fn) y jamas se invoca.
 */

#ifndef VESTA_JIT_NAKED_NATIVE_H
#define VESTA_JIT_NAKED_NATIVE_H

#include <cstdint>

namespace jit {

/**
 * @brief Registra el helper nativo @c vrt:naked_dispatch en el registro de
 *        funciones virtuales (FFI).  Lo invoca el bytecode emitido por el
 *        frontend (CALLN) para cada llamada a una funcion @Naked.  Debe
 *        llamarse una vez al arranque (junto a @c register_inline_asm_runner).
 */
void register_naked_dispatch_runner();

/**
 * @brief Indica si @p addr apunta DENTRO del code cache de funciones nativas
 *        compiladas al vuelo (naked-native).  Lo usa @c exec_instr_callvmr /
 *        @c callvm para decidir si un puntero de funcion es una entrada NATIVA
 *        (a invocar con ABI del host via @c invoke_native_unchecked) o una
 *        direccion VM de bytecode (dispatch normal).
 */
bool is_naked_native_addr(uint64_t addr);

/**
 * @brief Devuelve (compilando al vuelo si hace falta) la direccion NATIVA de
 *        la funcion Vex @p name.  La usa el lowering del cast a puntero de
 *        funcion (`(fn(...)) foo`) en interp/JIT para que un puntero a funcion
 *        que puede fluir a codigo nativo (asm @Naked o callvmr nativo) porte la
 *        direccion nativa en vez de la VA de bytecode.  0 si no se pudo.
 */
extern "C" uint64_t vrt_naked_fnaddr(uint64_t proc, uint64_t name_hash);

/**
 * @brief Registra @c vrt:naked_fnaddr (ver arriba).
 */
void register_naked_fnaddr_runner();

/**
 * @brief FNV-1a 64-bit del NOMBRE de una funcion @Naked.  Clave estable que
 *        (a) el lowering empaqueta como inmediato del CALLN al dispatcher y
 *        (b) el dispatcher usa para localizar el @c IrFunction en el
 *        @c Executable cargado.
 */
inline uint64_t fnv1a64_name(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (const char *p = s; *p; ++p) {
        h ^= static_cast<uint64_t>(static_cast<unsigned char>(*p));
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace jit

#endif // VESTA_JIT_NAKED_NATIVE_H
