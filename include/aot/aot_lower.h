/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file aot/aot_lower.h
 * @brief Phase AOT.2 -- re-bajada de ops sintetizadas a CALL a simbolos externos.
 *
 * Pase IR->IR (independiente del target) que el driver @c -m aot ejecuta ANTES
 * del codegen.  Reescribe las operaciones que el COMPILADOR sintetiza por
 * semantica del lenguaje (no las escribio el usuario) a llamadas a SiMBOLOS
 * EXTERNOS, para que el @c vreg_select (que ya soporta @c CALL) las compile sin
 * tocar el backend:
 *
 *   - RAW_ALLOC(size)  -> call <alloc>(size)   (convencion: "malloc")
 *   - RAW_FREE(ptr)    -> call <free>(ptr)      (convencion: "free"); o NOP si
 *                         ptr proviene de un ALLOCA (pila, no se libera).
 *   - PANIC(msg,len)   -> call <panic>()        (convencion: "abort")
 *
 * IMPORTANTE -- el AOT NO depende de libc.  Los nombres @c malloc / @c free /
 * @c abort son SOLO una CONVENCION: el objeto emitido contiene una REFERENCIA a
 * un simbolo EXTERNO indefinido que resuelve el LINKER.  Quien enlaza decide:
 *   - contra libc (gcc prog.o) -> usa la libc del sistema;
 *   - en freestanding / kernel -> el usuario aporta sus propios simbolos
 *     (p.ej. un wrapper @c malloc sobre @c kmalloc), o los renombra via
 *     @c @AllocatorOverride / @c @PanicHandler (AOT.2.d).
 * El .o/.obj jamas arrastra libc por si mismo.
 *
 * Sin punteros colgantes ni leaks: el simbolo de liberacion SOLO se invoca
 * sobre punteros heap reales; un @c raw_free cuyo operando proviene de un
 * @c ALLOCA promovido por el optimizador (escape analysis) se elimina (la pila
 * no se libera -> nada de dangling).
 */

#ifndef AOT_AOT_LOWER_H
#define AOT_AOT_LOWER_H

#include <string>

namespace ir { struct IrModule; }

namespace aot {

    /**
     * @brief Nombres de los simbolos externos a los que se re-bajan las ops
     *        sintetizadas.  Por defecto la CONVENCION C (malloc/free/abort);
     *        @c @AllocatorOverride / @c @PanicHandler (AOT.2.d) los sustituyen.
     */
    struct AotLowerConfig {
        std::string alloc_sym = "malloc";  ///< RAW_ALLOC -> call <alloc_sym>
        std::string free_sym  = "free";    ///< RAW_FREE  -> call <free_sym>
        std::string panic_sym = "abort";   ///< PANIC     -> call <panic_sym>
        /// AOT.2.d: si true, @c PANIC pasa (msg_addr, len) al @c panic_sym
        /// (un @c @PanicHandler con esa firma).  Si false (default, abort),
        /// se descarta el mensaje (abort no toma argumentos).
        bool        panic_takes_msg = false;
    };

    /**
     * @brief Reescribe in-place las ops sintetizadas del modulo a CALL externos.
     * @param mod Modulo IR (se modifica).
     * @param cfg Nombres de simbolos (default = convencion C, overridable).
     */
    void aot_lower_runtime(ir::IrModule &mod, const AotLowerConfig &cfg = {});

} // namespace aot

#endif // AOT_AOT_LOWER_H
