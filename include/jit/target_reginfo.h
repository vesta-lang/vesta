/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/target_reginfo.h
 * @brief Descriptor de registros por arquitectura para el register allocator
 *        (Phase D.7).  Capa 2 del diseno (ver doc/REGALLOC.md).
 *
 * Esta es la UNICA pieza que cambia por arquitectura desde el punto de vista
 * del allocator.  El allocator CORE es generico: lee de aqui que registros
 * hay en cada clase, cuales son scratch (caller-saved) vs preservados
 * (callee-saved), cuales estan reservados, la convencion de llamada y el
 * ancho de puntero.  Nunca usa literales @c MReg::* .
 *
 * Portar a x86-32 / ARMv7 / AArch64 / RISC-V = anyadir una funcion
 * @c target_<arch>() que devuelva su @c TargetRegInfo (mas su selector y
 * encoder, que son independientes del allocator).
 */

#ifndef VESTA_JIT_TARGET_REGINFO_H
#define VESTA_JIT_TARGET_REGINFO_H

#include "jit/machine_ir.h"

#include <cstdint>
#include <vector>

namespace jit {

/**
 * @struct TargetRegInfo
 * @brief Descripcion completa del banco de registros de un target.
 *
 * Todas las tablas estan indexadas por @c (size_t)RegClass (0=GP, 1=FP),
 * con @c RegClass::COUNT entradas.  Los valores son ids fisicos (el
 * mismo espacio que @c MReg en x86; otro target usa su propia numeracion
 * pero el allocator los trata como @c uint8_t opacos).
 */
struct TargetRegInfo {
    /// Ancho de puntero/registro en bytes: 8 (x86-64/AArch64) o 4
    /// (x86-32/ARMv7).  De aqui derivan el tamano de los spill slots.
    uint8_t pointer_size = 8;

    /// True si la ISA es de DOS operandos (x86: `add rd, rs` -> rd=rd+rs)
    /// y por tanto necesita el pase de two-address legalization.  ARM,
    /// AArch64 y RISC-V son de tres operandos -> false.
    bool is_two_address = true;

    static constexpr size_t NCLASS = static_cast<size_t>(RegClass::COUNT);

    /// Registros ASIGNABLES por clase (excluye reservados).  El orden
    /// es la preferencia de asignacion del allocator.
    std::vector<uint8_t> allocatable[NCLASS];
    /// Subconjunto caller-saved (scratch): se PIERDEN al cruzar un CALL.
    /// El allocator los prefiere para valores de vida corta.
    std::vector<uint8_t> caller_saved[NCLASS];
    /// Subconjunto callee-saved: hay que push/pop en prologue/epilogue
    /// si se usan.  El allocator los prefiere para valores que cruzan
    /// llamadas (evita salvarlos alrededor de cada CALL).
    std::vector<uint8_t> callee_saved[NCLASS];
    /// Registros SCRATCH reservados para el rewrite (materializar
    /// spills / legalizacion two-address).  NUNCA asignados por el
    /// allocator (excluidos de @c allocatable).  Solo viven dentro de
    /// una instruccion, nunca a traves de un CALL -> caller-saved es ok.
    std::vector<uint8_t> scratch[NCLASS];
    /// Orden de registros para pasar argumentos (convencion de llamada
    /// del host) por clase.
    std::vector<uint8_t> arg_regs[NCLASS];
    /// Registro donde queda el valor de retorno por clase.
    uint8_t ret_reg[NCLASS] = {0, 0};
    /// Registros reservados que el allocator NUNCA asigna (frame +
    /// punteros fijos del ABI de la VM).
    std::vector<uint8_t> reserved;

    /** @brief True si @p r es asignable en la clase @p cls. */
    bool is_allocatable(RegClass cls, uint8_t r) const noexcept {
        const auto &v = allocatable[static_cast<size_t>(cls)];
        for (uint8_t x : v)
            if (x == r) return true;
        return false;
    }
    /** @brief True si @p r es callee-saved en la clase @p cls. */
    bool is_callee_saved(RegClass cls, uint8_t r) const noexcept {
        const auto &v = callee_saved[static_cast<size_t>(cls)];
        for (uint8_t x : v)
            if (x == r) return true;
        return false;
    }
    /** @brief Numero de fisicos asignables de la clase @p cls. */
    size_t num_allocatable(RegClass cls) const noexcept {
        return allocatable[static_cast<size_t>(cls)].size();
    }
};

/**
 * @brief Descriptor del target x86-64 con la VM_ABI de VestaVM.
 *
 * Convencion VM_ABI: @c RBX = ProcessVM* (fijo durante toda la funcion
 * JIT) -> RESERVADO.  @c RSP/@c RBP = frame -> reservados.  El resto de
 * GP son asignables.  La clase FP (XMM) se describe COMPLETA aunque el
 * allocator v1 no la use todavia (camino preparado para la fase XMM).
 *
 * Los @c arg_regs siguen el ABI nativo del host (SysV en POSIX, Win64 en
 * Windows) porque los CALL a runtime entries (@c vrt_*) usan @c extern
 * "C" con esa convencion.
 *
 * @return Referencia a una instancia construida una sola vez (thread-safe
 *         por inicializacion de static local en C++11).
 */
/**
 * @brief Construye el @c TargetRegInfo x86-64 para el ABI dado.
 * @param sysv  true = System V AMD64 (Linux/ELF); false = Win64 (Windows/PE).
 *
 * IMPORTANTE (AOT cross-target): el ABI es del TARGET (formato de salida),
 * NO del host.  Un @c .so/.dll o @c .o que exporta funciones invocadas
 * externamente DEBE usar el ABI de su plataforma (SysV para ELF/Linux, Win64
 * para PE/Windows): los @c arg_regs y la particion caller/callee-saved
 * difieren.  El JIT en proceso usa el ABI del host (ver
 * @c target_x86_64_vm_abi).
 */
inline TargetRegInfo build_x86_64_target(bool sysv) {
    TargetRegInfo t;
    t.pointer_size = 8;
    t.is_two_address = true;

    const size_t GP = static_cast<size_t>(RegClass::GP);
    const size_t FP = static_cast<size_t>(RegClass::FP);
    auto id = [](MReg r) { return reg_id(r); };

    /* R10/R11 reservados como SCRATCH del rewrite (no asignables). */
    t.scratch[GP] = {id(MReg::R10), id(MReg::R11)};
    if (!sysv) {
        /* Win64 volatiles: RAX RCX RDX R8 R9. */
        t.caller_saved[GP] = {id(MReg::RAX), id(MReg::RCX), id(MReg::RDX),
                              id(MReg::R8), id(MReg::R9)};
        /* Win64 no-volatiles asignables: RSI RDI R12-R15. */
        t.callee_saved[GP] = {id(MReg::RSI), id(MReg::RDI), id(MReg::R12),
                              id(MReg::R13), id(MReg::R14), id(MReg::R15)};
    } else {
        /* SysV volatiles: RAX RCX RDX RSI RDI R8 R9. */
        t.caller_saved[GP] = {id(MReg::RAX), id(MReg::RCX), id(MReg::RDX),
                              id(MReg::RSI), id(MReg::RDI), id(MReg::R8),
                              id(MReg::R9)};
        /* SysV no-volatiles asignables: R12-R15. */
        t.callee_saved[GP] = {id(MReg::R12), id(MReg::R13), id(MReg::R14),
                              id(MReg::R15)};
    }
    t.allocatable[GP] = t.caller_saved[GP];
    t.allocatable[GP].insert(t.allocatable[GP].end(),
                             t.callee_saved[GP].begin(),
                             t.callee_saved[GP].end());
    t.ret_reg[GP] = id(MReg::RAX);

    for (int i = 16; i <= 31; ++i) {
        t.allocatable[FP].push_back(static_cast<uint8_t>(i));
        t.caller_saved[FP].push_back(static_cast<uint8_t>(i));
    }
    t.ret_reg[FP] = id(MReg::XMM0);
    t.reserved = {id(MReg::RSP), id(MReg::RBP), id(MReg::RBX)};

    if (!sysv) {
        /* Win64: args enteros en RCX RDX R8 R9; float en XMM0..3. */
        t.arg_regs[GP] = {id(MReg::RCX), id(MReg::RDX), id(MReg::R8),
                          id(MReg::R9)};
        t.arg_regs[FP] = {id(MReg::XMM0), id(MReg::XMM1), id(MReg::XMM2),
                          id(MReg::XMM3)};
    } else {
        /* SysV AMD64: args enteros en RDI RSI RDX RCX R8 R9; float XMM0..7. */
        t.arg_regs[GP] = {id(MReg::RDI), id(MReg::RSI), id(MReg::RDX),
                          id(MReg::RCX), id(MReg::R8),  id(MReg::R9)};
        t.arg_regs[FP] = {id(MReg::XMM0), id(MReg::XMM1), id(MReg::XMM2),
                          id(MReg::XMM3), id(MReg::XMM4), id(MReg::XMM5),
                          id(MReg::XMM6), id(MReg::XMM7)};
    }
    return t;
}

/**
 * @brief Construye el @c TargetRegInfo x86-32 (modo protegido, kernels).
 *
 * Solo 8 GP (eax=0..edi=7; no hay r8-r15) -> pointer_size=4.  Convencion
 * REGPARM(3) (estilo GCC @c -mregparm=3): args enteros en EAX, EDX, ECX;
 * retorno en EAX; callee-saved EBX, ESI, EDI, EBP.  Se elige regparm (no
 * cdecl con args en pila) para reusar el modelo de @c arg_regs del selector
 * HOST_LEAF sin implementar paso por pila -> cubre kernel freestanding con
 * funciones leaf + llamadas internas (las externas/BIOS van por asm).
 *
 * ESP/EBP reservados (frame).  ESI/EDI reservados como SCRATCH del rewrite
 * (materializar spills + two-address).  Asignables: EAX ECX EDX EBX (4).
 * FP (XMM) vacio en v1: el subset 32-bit es entero (i32/u32/ptr32); las ops
 * float no aparecen.
 */
inline TargetRegInfo build_x86_32_target() {
    TargetRegInfo t;
    t.pointer_size = 4;
    t.is_two_address = true;

    const size_t GP = static_cast<size_t>(RegClass::GP);
    auto id = [](MReg r) { return reg_id(r); };

    /* ESI/EDI = scratch del rewrite (no asignables). */
    t.scratch[GP] = {id(MReg::RSI), id(MReg::RDI)};
    /* Volatiles regparm: EAX ECX EDX. */
    t.caller_saved[GP] = {id(MReg::RAX), id(MReg::RCX), id(MReg::RDX)};
    /* No-volatiles asignables: EBX. */
    t.callee_saved[GP] = {id(MReg::RBX)};
    t.allocatable[GP] = t.caller_saved[GP];
    t.allocatable[GP].insert(t.allocatable[GP].end(),
                             t.callee_saved[GP].begin(),
                             t.callee_saved[GP].end());
    t.ret_reg[GP] = id(MReg::RAX);

    /* FP vacio (sin float en v1). */
    t.reserved = {id(MReg::RSP), id(MReg::RBP)};

    /* regparm(3): EAX, EDX, ECX. */
    t.arg_regs[GP] = {id(MReg::RAX), id(MReg::RDX), id(MReg::RCX)};
    return t;
}

/** @brief @c TargetRegInfo x86-32 (cacheado). */
inline const TargetRegInfo &target_x86_32() {
    static const TargetRegInfo info = build_x86_32_target();
    return info;
}

/** @brief @c TargetRegInfo x86-64 para el ABI @p sysv (cacheado por ABI). */
inline const TargetRegInfo &target_x86_64_abi(bool sysv) {
    static const TargetRegInfo sysv_info = build_x86_64_target(true);
    static const TargetRegInfo win_info = build_x86_64_target(false);
    return sysv ? sysv_info : win_info;
}

/** @brief @c TargetRegInfo del ABI del HOST (para el JIT en proceso). */
inline const TargetRegInfo &target_x86_64_vm_abi() {
#if defined(_WIN32)
    return target_x86_64_abi(/*sysv=*/false);
#else
    return target_x86_64_abi(/*sysv=*/true);
#endif
}

} // namespace jit

#endif // VESTA_JIT_TARGET_REGINFO_H
