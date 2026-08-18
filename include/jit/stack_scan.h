/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/stack_scan.h
 * @brief Stack walker preciso para JIT frames usando stackmaps.
 *
 * = Diseno =
 *
 * Cuando el GC quiere encontrar todos los GC roots de un proceso,
 * camina la cadena de frames @c RBP -> [RBP] -> [[RBP]] -> ...  Por
 * cada frame:
 *
 *   1. Captura el RIP del caller (almacenado en @c [RBP+8] por la
 *      convencion x86-64: @c push rbp pone caller's saved RBP, y
 *      la @c call que vino antes puso el return address en @c [RBP+8]).
 *   2. Lookup en @c JitRegistry: ¿es @p rip parte de una funcion JIT?
 *      Si SI: este es un JIT frame, usar stackmap para precise scan.
 *      Si NO: este es un interp frame (o C runtime), saltar (scan
 *      conservativo cubrira sus slots).
 *   3. Avanzar al siguiente frame: @c rbp = [rbp].  Parar cuando
 *      @c rbp == nullptr o sale del rango del stack.
 *
 * = Coexistencia con scan conservativo =
 *
 * (integration): este scan corre EN PARALELO con
 * el conservativo (@c gc_heap::scan_stack_roots).  Los GC roots de
 * JIT frames se marcan precise + tambien conservativo los marca
 * "por accidente" pero como mismo target -> sin efecto.
 *
 * (futuro): el conservativo se restringe a NO escanear
 * rangos de stack cubiertos por JIT frames -> elimina falsos
 * positivos por completo.
 */

#ifndef VESTA_JIT_STACK_SCAN_H
#define VESTA_JIT_STACK_SCAN_H

#include <cstdint>

#include "jit/machine_ir.h"

namespace jit {

/**
 * @struct JitScanStats
 * @brief Estadisticas de un scan de JIT frames.  Util para
 *        comparar precise vs conservative en la fase 1.
 */
struct JitScanStats {
    uint32_t frames_walked = 0;  ///< total frames en la cadena RBP
    uint32_t jit_frames = 0;     ///< frames JIT (con stackmap)
    uint32_t non_jit_frames = 0; ///< frames no-JIT (saltados aqui)
    uint32_t handles_marked = 0; ///< slots GC marcados precise
    uint32_t hostptr_marked = 0; ///< slots host_ptr marcados precise
};

/**
 * @brief Callback invocado por @c scan_jit_frames para cada root
 *        encontrado.  El caller (GcHeap o test) decide que hacer
 *        con la informacion (marcar handle, encolar en worklist,
 *        log para debug, etc.).
 *
 * @param ctx       contexto opaco del caller.
 * @param value     valor leido del slot (uint64).
 * @param kind      tipo segun el stackmap.
 * @param slot_addr direccion del slot (para debugging / forward refs).
 */
using JitRootCallback = void (*)(void *ctx, uint64_t value, StackmapGcKind kind,
                                 const uint8_t *slot_addr);

/**
 * @brief Lee 8 bytes (uint64) desde @p ptr.  Usado por el walker.
 */
inline uint64_t read_u64(const uint8_t *ptr) noexcept {
    uint64_t v;
    __builtin_memcpy(&v, ptr, sizeof(v));
    return v;
}

/**
 * @brief Walk de la cadena RBP de un thread y precise-scan de los
 *        JIT frames encontrados.
 *
 * El callback @p cb se invoca para cada slot GC del stackmap,
 * con el valor leido y su categoria.
 *
 * @param cb         callback a invocar por cada root encontrado.
 * @param cb_ctx     contexto opaco para @p cb.
 * @param rbp_top    RBP actual del thread.  En el handler de
 *                   safepoint este es @c __builtin_frame_address(0)
 *                   del caller del handler.
 * @param stack_low  limite inferior del stack (no leer mas abajo).
 * @param stack_high limite superior del stack (no leer mas arriba).
 * @return estadisticas del scan.
 */
JitScanStats scan_jit_frames(JitRootCallback cb, void *cb_ctx,
                             const uint8_t *rbp_top, const uint8_t *stack_low,
                             const uint8_t *stack_high) noexcept;

/**
 * @brief Walk PRECISO por TAMANO DE FRAME (modelo LLVM statepoint) para el GC
 *        de AOT.
 *
 * A diferencia de @c scan_jit_frames NO camina la cadena RBP: reconstruye el
 * RBP de cada frame como @c RSP_del_llamador + frame_size (leido del
 * @c JitFunctionInfo) y avanza al llamador con @c next_sp = rbp + 16 y
 * @c next_pc = [rbp+8].  Al no leer nunca @c [rbp] (el saved-RBP), es robusto
 * ante frames que omiten el frame pointer (-fomit-frame-pointer) o inlining.
 *
 * Arranca en el primer frame Vesta REAL, cuyo (PC, SP) se captura en la
 * frontera C<-Vesta (ver @c GcHeap::set_aot_scan_boundary), saltando los frames
 * C++ no-walkables de libvesta_gc.  Sube hasta que un PC no pertenece a ninguna
 * funcion Vesta registrada (CRT / _start) o hasta @c MAX_FRAMES.
 *
 * @param cb        callback por cada root GC encontrado.
 * @param cb_ctx    contexto opaco para @p cb.
 * @param start_pc  PC de retorno al primer frame Vesta (return address).
 * @param start_sp  RSP de ese frame Vesta justo antes del @c call al GC.
 * @return estadisticas del scan (jit_frames = frames Vesta recorridos).
 */
JitScanStats scan_aot_frames(JitRootCallback cb, void *cb_ctx,
                             uint64_t start_pc, uint64_t start_sp) noexcept;

} // namespace jit

#endif // VESTA_JIT_STACK_SCAN_H
