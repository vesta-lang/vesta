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
 * TODAS sus callees Vesta alcanzables via @c vreg_compile_native (ABI HOST_LEAF,
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
#include <string>

namespace runtime {
class ProcessVM;
}

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
 *        la funcion Vesta @p name.  La usa el lowering del cast a puntero de
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

/* ===================================================================== */
/* FN.3: fibras nativas en JIT                                            */
/* ===================================================================== */

/**
 * @brief Compila (o recupera de cache) la funcion Vesta @p name como una entrada
 *        NATIVA HOST_LEAF (respetando @c is_naked), con sus relocs resueltos.
 *        Wrapper publico y thread-safe de la maquinaria @Naked interna.
 *
 * Lo usa el force-eager del grafo de fibra para materializar @c __vx_swapctx
 * (context-switch @Naked) antes de compilar los cuerpos de fibra, de modo que
 * el vreg pueda emitir un CALL nativo directo a el para @c IrOp::SWAPCTX.
 *
 * @return direccion nativa (en el code cache naked), o 0 si no se pudo (p.ej.
 *         arquitectura sin backend x86-64 -> fibras-en-JIT deshabilitadas).
 */
uint64_t compile_naked_native(runtime::ProcessVM *vm, const std::string &name);

/**
 * @brief Helper de runtime para @c IrOp::CALLIND en JIT (FN.3 pieza 2).
 *
 * Replica EXACTAMENTE @c exec_instr_callvmr del interprete: dado @p addr (el
 * valor del puntero a funcion) distingue por rango y despacha:
 *   - naked-native (HOST_LEAF)  -> @c invoke_native_unchecked (ABI host).
 *   - jit_code VM_ABI            -> @c enter_jit (VM_ABI).
 *   - VA de bytecode             -> compile-on-demand -> enter_jit; ultimo
 *                                   recurso: ejecutar el bytecode.
 * Convencion (ya establecida por el caller JIT): args en
 * @c proc->registers.regs[1..N], argc en @c regs[15], resultado en @c regs[0].
 */
extern "C" void vrt_callind(uint64_t proc, uint64_t addr);

/**
 * @brief FN.3: extern `vrt:jit_active` -> 1 si hay codigo JIT activo (el
 *        proceso corre en modo JIT), 0 si es interprete puro.  Lo usa
 *        `fiber_init` para elegir el modelo de fibra (JIT: pila/ctx HOST +
 *        trampolin + proc; interp: pila/ctx en memoria VM).  En AOT este
 *        extern se pliega a 0 en el lowering (no llega aqui).
 */
extern "C" int32_t vrt_jit_active(void);

/**
 * @brief FN.3: extern `vrt:getproc` -> ProcessVM* del proceso en ejecucion
 *        (via TLS, como `vx_get_native_thunk`).  Lo usa `fiber_init` en la
 *        rama JIT para poner `proc` en el ctx de la fibra (rbx del entry
 *        VM_ABI).  En AOT se pliega a 0 en el lowering.
 */
extern "C" uint64_t vrt_getproc(void);

/**
 * @brief FN.3: extern `vrt:fiber_jit_ctx(entry)` -> construye en memoria HOST el
 *        contexto de una fibra para el context-switch nativo (JIT).
 *
 * Reserva ctx (152 B) + pila (64 KiB) con malloc (memoria host), materializa el
 * trampolin `__fiber_trampoline` nativo, y rellena el ctx con el layout que
 * `__vx_swapctx` espera al PRIMER arranque: PC=trampolin, SP=BP=cima de la
 * pila host, rbx=proc (recuperado via TLS), r12=@p entry (jit_code nativo del
 * cuerpo, que @c fiber_entry resuelve via la pieza 1).  El trampolin pone proc
 * en el arg-reg y salta al entry VM_ABI.  Devuelve la direccion del ctx host.
 * Solo se llama en JIT (la rama `if(jit_active())` del setup de fibras); en AOT
 * se pliega a 0 en el lowering.
 */
extern "C" uint64_t vrt_fiber_jit_ctx(uint64_t entry);

/**
 * @brief FN.3: extern `vrt:fiber_jit_scratch()` -> ctx host vacio (152 B a cero)
 *        para el scheduler/main (se rellena en el primer swap-out).  Solo JIT.
 */
extern "C" uint64_t vrt_fiber_jit_scratch(void);

/**
 * @brief Registra `vrt:jit_active`, `vrt:getproc`, `vrt:fiber_jit_ctx` y
 *        `vrt:fiber_jit_scratch` en el registro de funciones virtuales (FFI).
 *        Llamar una vez al arranque (junto a `register_naked_fnaddr_runner`).
 */
void register_fiber_runtime_runner();

} // namespace jit

#endif // VESTA_JIT_NAKED_NATIVE_H
