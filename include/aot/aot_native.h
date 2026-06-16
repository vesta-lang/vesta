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
 * @file aot/aot_native.h
 * @brief Phase AOT.3 Paso 2 -- sintesis del @c _start nativo (arch-portable).
 *
 * El cuerpo de cada funcion lo genera el back-end de produccion (path vreg ->
 * @c TargetRegInfo + selector + encoder), que YA es portable por arquitectura:
 * portar a x86-32/x86-16/ARM/AArch64/RISC-V = anyadir su @c target_<arch>() +
 * selector + encoder (el register allocator core es generico).
 *
 * Lo unico ESPECIFICO de cada arch+formato que vive aqui es el @c _start: el
 * punto de entrada del proceso, que llama a @c main y termina el proceso con
 * su codigo de retorno.  Esto depende de:
 *   - la ISA (como se codifica el @c call y el "terminar proceso"), y
 *   - la plataforma de salida (PE termina via @c kernel32!ExitProcess por la
 *     IAT; ELF freestanding termina via @c syscall @c exit(60)).
 *
 * Por eso la sintesis del stub se despacha por @c (AotArch, ObjFormat).  Hoy
 * solo esta implementado x86-64; anyadir una arch nueva = un @c case mas en
 * @c aot_make_start_stub (sin tocar el resto del pipeline AOT).
 */

#ifndef AOT_AOT_NATIVE_H
#define AOT_AOT_NATIVE_H

#include "aot/object_writer.h"  // ObjFormat

#include <cstdint>
#include <string>
#include <vector>

namespace aot {

    /**
     * @brief Arquitectura objetivo del codegen AOT.
     *
     * Cada valor implica un @c TargetRegInfo + selector + encoder propios para
     * el cuerpo de las funciones, y un @c case en @c aot_make_start_stub para
     * el @c _start.  Solo @c X86_64 esta implementado en el hito inicial; el
     * resto son extensiones futuras (el enum los reserva para no romper la ABI
     * del modulo cuando lleguen).
     */
    enum class AotArch : uint8_t {
        X86_64 = 0,  ///< x86-64 (AMD64).  Implementado.
        X86_32 = 1,  ///< x86 de 32 bits (i386).  Futuro.
        X86_16 = 2,  ///< x86 de 16 bits (8086/real mode).  Futuro.
        ARM64  = 3,  ///< AArch64.  Futuro.
        ARM32  = 4,  ///< ARMv7.  Futuro.
        RISCV64 = 5, ///< RISC-V 64.  Futuro.
    };

    /**
     * @brief Stub @c _start sintetizado para un (arch, formato) dado.
     *
     * El @c _start asume que @c main viene INMEDIATAMENTE DESPUES del stub en la
     * misma seccion @c .text (en el offset @c bytes.size()); el @c call a @c main
     * ya queda resuelto (desplazamiento relativo) dentro de @c bytes.  Para PE,
     * la terminacion del proceso es un @c call indirecto a @c ExitProcess via la
     * IAT, que el caller debe registrar con @c ObjectWriter::add_import_call
     * usando @c import_call_off (offset del @c FF @c 15 dentro de @c bytes).
     */
    struct StartStub {
        bool                 ok = false;          ///< true si el (arch,fmt) esta soportado.
        std::string          err;                 ///< motivo si @c !ok.
        std::vector<uint8_t> bytes;               ///< codigo del @c _start (entrada en offset 0).
        bool                 has_import_call = false; ///< true => terminar via import (PE).
        uint64_t             import_call_off = 0;  ///< offset del @c FF @c 15 a parchear.
        std::string          import_dll;           ///< "KERNEL32.dll" (si @c has_import_call).
        std::string          import_func;          ///< "ExitProcess" (si @c has_import_call).
    };

    /**
     * @brief Sintetiza el @c _start para @p arch + @p fmt.
     *
     * El stub llama a @c main (situado justo despues, en @c bytes.size()) y
     * termina el proceso con @c main()->eax (PE: @c ExitProcess via IAT; ELF:
     * @c syscall @c exit).  El @c call a @c main queda ya parcheado para apuntar
     * al offset @c bytes.size().
     *
     * @param arch Arquitectura objetivo.
     * @param fmt  Formato de salida (PE/ELF).
     * @return @c StartStub con @c ok=true y los bytes, o @c ok=false + @c err si
     *         la combinacion (arch,fmt) no esta soportada todavia.
     */
    StartStub aot_make_start_stub(AotArch arch, ObjFormat fmt);

} // namespace aot

#endif // AOT_AOT_NATIVE_H
