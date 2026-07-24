/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/vm_target.h
 * @brief La ISA de la PROPIA VM (r0-r15) descrita como un target mas.
 *
 * POR QUE VIVE EN @c codegen/ Y NO EN @c jit/.  El camino del INTERPRETE no es
 * el JIT.  Aqui esta el nivel que comparten los TRES modos -- interprete, JIT y
 * AOT -- igual que @c codegen/regalloc.h o @c codegen/timeline_builder.h, que
 * dejaron de ser del JIT cuando pasaron a servir a todos.  Meter la descripcion
 * de la VM bajo @c jit/ ataria el interprete al JIT sin ninguna razon tecnica.
 *
 * POR QUE EXISTE.  Hoy el emisor `.vel` usa su propio asignador
 * (@c ir::allocate_regs) mientras JIT y AOT usan @c codegen::rbank: DOS
 * asignadores para el mismo problema, con el conocimiento (taxonomia de spills,
 * Recovery, next-use/Belady, telemetria) llegando solo a uno.  Describiendo la
 * ISA de la VM como DATOS, el mismo allocator sirve a los tres modos y la
 * logica deja de duplicarse -- que es la misma razon por la que el JIT jubilo
 * su @c linear_scan en favor de rbank.
 *
 * ABI DE LA VM.  Es la que el emisor ya asume; aqui queda escrita como datos en
 * vez de como constantes dispersas por el codigo:
 *   - r0        valor de retorno
 *   - r1-r12    argumentos / asignables
 *   - r13, r14  scratch del emisor (materializan operandos DERRAMADOS)
 *   - r15       argc de las llamadas
 *
 * DEPENDENCIA.  Usa @c jit::TargetRegInfo porque ese tipo es HOY el descriptor
 * que consume rbank; su ubicacion bajo @c jit/ es herencia de cuando el
 * allocator era solo del JIT.  Cuando se mueva a @c codegen/ (o se sustituya
 * por el @c TargetTraits neutral, que ya existe), este fichero solo cambia el
 * include.
 */

#ifndef VESTA_CODEGEN_VM_TARGET_H
#define VESTA_CODEGEN_VM_TARGET_H

#include "jit/target_reginfo.h"

#include <cstdint>

namespace codegen {

/**
 * @brief Construye el descriptor de registros de la VM.
 *
 * @param reserve_scratch  si false, r13/r14/r15 pasan a ser ASIGNABLES.  Es el
 *        gancho para la reserva POR DEMANDA -- r15 solo hace falta si la
 *        funcion llama, y r13 solo si hay derrames -- con el mismo patron que
 *        @c reserve_vec_acc usa para liberar XMM10-13 en funciones sin ops
 *        vectoriales.  Reservarlos siempre cuesta 3 de 16 registros incluso
 *        donde no se usan.
 *
 * NOTA SOBRE LAS LLAMADAS: en la VM un CALL no clobbea el banco del llamante
 * (cada frame tiene el suyo), asi que no hay caller-saved y @c crosses_call no
 * impone preferencia alguna -- todos los asignables sobreviven.  Modelarlo asi
 * es DESCRIBIR esta maquina, no calcar la de x86.
 */
inline jit::TargetRegInfo build_vm_target(bool reserve_scratch = true) {
    jit::TargetRegInfo t;
    t.pointer_size = 8;
    t.is_two_address = true; // `add rd, rs` -> rd = rd + rs

    const size_t GP = static_cast<size_t>(jit::RegClass::GP);
    const uint8_t last_alloc = reserve_scratch ? 12 : 15;
    for (uint8_t r = 0; r <= last_alloc; ++r) t.allocatable[GP].push_back(r);
    if (reserve_scratch) {
        t.scratch[GP] = {13, 14};
        t.reserved = {15}; // argc
    }
    // Convencion que el emisor pre-asigna: retorno en r0, argumentos r1-r12.
    for (uint8_t r = 1; r <= 12; ++r) t.arg_regs[GP].push_back(r);
    t.ret_reg[GP] = 0;
    // Nada se pierde al cruzar un CALL -> todos "sobreviven" (callee_saved es
    // lo que el modelo entiende por eso).
    t.callee_saved[GP] = t.allocatable[GP];
    return t;
}

/** @brief Instancia cacheada del target de la VM.  @see build_vm_target. */
inline const jit::TargetRegInfo &target_vm(bool reserve_scratch = true) {
    static const jit::TargetRegInfo with_scratch = build_vm_target(true);
    static const jit::TargetRegInfo no_scratch = build_vm_target(false);
    return reserve_scratch ? with_scratch : no_scratch;
}

} // namespace codegen

#endif // VESTA_CODEGEN_VM_TARGET_H
