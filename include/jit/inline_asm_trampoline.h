/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/inline_asm_trampoline.h
 * @brief Phase AS inc.6: trampoline nativo para ejecutar inline-asm desde el
 *        INTERPRETE (modo `-m vm`), SIN el compilador JIT.
 *
 * El interprete no puede materializar asm de la CPU host; pero SI puede LLAMAR
 * a una funcion nativa.  La estrategia (acordada con el usuario): envolver el
 * bloque asm como una funcion nativa que el interprete invoca, pasando los
 * registros via un array @c ctx[16] (indexado por id de registro GP host:
 * rax=0, rcx=1, rdx=2, rbx=3, rsp=4(no usado), rbp=5, rsi=6, rdi=7, r8=8 ..
 * r15=15).
 *
 * El trampoline es un envoltorio GENERICO (mismo prologo/epilogo para cualquier
 * asm): salva los callee-saved, carga los 16 GP host desde @c ctx, ejecuta el
 * asm del usuario, guarda los 16 GP host de vuelta a @c ctx, restaura los
 * callee-saved.  El @c ctx vive en la PILA durante el asm (reentrante; ademas
 * resuelve el clobber de rbx/rbp -- el asm puede pisarlos, se recargan de la
 * pila).  El ENSAMBLADO usa @c vex::g_asm_backend (Keystone) -- NO el selector/
 * regalloc del JIT, asi que funciona en interp puro.
 *
 * Convencion del trampoline emitido: @c void tramp(uint64_t ctx[16]).  El caller
 * (el exec_fn del interp, o un test) escribe @c ctx[reg] para las entradas,
 * llama, y lee @c ctx[reg] para las salidas.
 */

#ifndef VESTA_JIT_INLINE_ASM_TRAMPOLINE_H
#define VESTA_JIT_INLINE_ASM_TRAMPOLINE_H

#include <cstdint>
#include <string>

namespace jit {

    class CodeCache;

    /** @brief Firma del trampoline emitido: recibe el array ctx[16]. */
    using AsmTrampolineFn = void (*)(uint64_t *ctx);

    /**
     * @brief Construye (ensambla via Keystone + aloca ejecutable) un trampoline
     *        nativo que ejecuta @p user_asm marshalizando los 16 GP host desde/
     *        hacia un array @c ctx[16].
     *
     * @param user_asm  Cuerpo NASM Intel del bloque asm (ya con comptime consts
     *                  sustituidas).  Debe estar balanceado en la pila y no
     *                  tocar rsp de forma desbalanceada.
     * @param cc        Code cache donde alojar los bytes ejecutables.
     * @param err       [out] mensaje de error si falla (nullptr para ignorar).
     * @return          Puntero al trampoline (@c AsmTrampolineFn), o @c nullptr
     *                  si no hay backend de ensamblado, el asm es invalido, o
     *                  la alocacion falla.
     */
    AsmTrampolineFn build_asm_trampoline(const std::string &user_asm,
                                         CodeCache &cc,
                                         std::string *err = nullptr);

} // namespace jit

#endif // VESTA_JIT_INLINE_ASM_TRAMPOLINE_H
