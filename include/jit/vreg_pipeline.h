/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/vreg_pipeline.h
 * @brief Orquestador del path de registros virtuales (Phase D.7, commit 5c).
 *
 * Encadena selector vreg -> intervalos -> linear-scan -> rewrite (VM_ABI) ->
 * encoder -> code cache -> registro en el JitRegistry.  Es el punto de entrada
 * que @c auto_jit invoca cuando @c VESTA_JIT_VREGS esta activo; si la funcion
 * no es del subset soportado por el selector vreg, devuelve @c nullptr y el
 * caller hace fallback al path de slots.
 */

#ifndef VESTA_JIT_VREG_PIPELINE_H
#define VESTA_JIT_VREG_PIPELINE_H

#include "jit/vreg_select.h"  // CallResolver

#include <cstdint>

namespace ir { struct IrFunction; }

namespace jit {

    class CodeCache;

    /**
     * @brief Compila @p fn por el path de registros virtuales (VM_ABI) y la
     *        registra en el JitRegistry.
     *
     * @param fn  Funcion IR a compilar.
     * @param cc  Code cache donde alojar el codigo nativo.
     * @return    Puntero al codigo nativo (invocable via @c enter_jit), o
     *            @c nullptr si la funcion no esta soportada por el selector
     *            vreg (el caller debe hacer fallback).
     */
    uint8_t *vreg_compile(const ir::IrFunction &fn, CodeCache &cc,
                          const CallResolver &resolve_call = {},
                          uint64_t callvirt_addr = 0,
                          uint64_t gc_deref_addr = 0,
                          uint64_t gc_handle_addr = 0,
                          uint64_t raw_alloc_addr = 0);

} // namespace jit

#endif // VESTA_JIT_VREG_PIPELINE_H
