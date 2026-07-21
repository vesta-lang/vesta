/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file aot/aot_native.cpp
 * @brief Implementacion de la sintesis del @c _start AOT (ver aot_native.h).
 */

#include "aot/aot_native.h"

namespace aot {

namespace {

/**
 * @brief @c _start x86-64 para PE: reserva shadow space, llama a main,
 *        y termina via @c kernel32!ExitProcess(eax) por la IAT.
 *
 *   48 83 EC 28        sub  rsp, 0x28        ; shadow space + alineamiento
 *   E8 rel32           call main             ; main justo despues del stub
 *   89 C1              mov  ecx, eax          ; arg0 = codigo de salida
 *   FF 15 disp32       call [rip+disp]        ; ExitProcess (IAT; a parchear)
 *   CC                 int3                   ; inalcanzable
 */
StartStub start_x86_64_pe() {
    StartStub s;
    s.bytes = {
        0x48, 0x83, 0xEC, 0x28,             // sub rsp, 0x28          (off 0)
        0xE8, 0x00, 0x00, 0x00, 0x00,       // call main (rel32@5)    (off 4)
        0x89, 0xC1,                         // mov ecx, eax           (off 9)
        0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, // call [rip+disp] (off 11, disp@13)
        0xCC                                // int3                   (off 17)
    };
    // AOT 2b: el `call main` (rel32 @5) NO se pre-parchea -> el driver
    // declara una reloc REL32 a la VA real de main (puede vivir en
    // cualquier seccion).  rel32 queda en 0 hasta que el writer resuelve.
    s.main_call_off = 5;
    s.has_import_call = true;
    s.import_call_off = 11; // offset del FF 15
    s.import_dll = "KERNEL32.dll";
    s.import_func = "ExitProcess";
    s.ok = true;
    return s;
}

/**
 * @brief @c _start x86-64 para ELF freestanding: llama a main y termina
 *        via @c syscall @c exit_group(231) con @c eax como codigo.  Sin libc
 *        ni imports -- valido para hosted Linux y kernels/bootloaders.
 *
 *   E8 rel32           call main             ; main justo despues del stub
 *   89 C7              mov  edi, eax          ; arg0 = codigo de salida
 *   B8 E7 00 00 00     mov  eax, 231          ; sys_exit_group
 *   0F 05              syscall
 *
 * NOTA: usamos exit_group (231) y NO exit (60).  El 60 termina solo el HILO
 * llamante; en un proceso single-thread el codigo de salida no siempre se
 * propaga al padre (p.ej. WSL2 reporta 0 aunque main devuelva 42).  exit_group
 * termina el proceso completo y propaga bien el exit-code.  En un kernel sin SO
 * debajo el _start nunca retorna a este exit, asi que el cambio es inocuo ahi.
 */
StartStub start_x86_64_elf() {
    StartStub s;
    s.bytes = {
        0xE8, 0x00, 0x00, 0x00, 0x00, // call main (rel32@1)          (off 0)
        0x89, 0xC7,                   // mov edi, eax                 (off 5)
        0xB8, 0xE7, 0x00, 0x00, 0x00, // mov eax, 231 (exit_group)    (off 7)
        0x0F, 0x05                    // syscall                      (off 12)
    };
    // AOT 2b: `call main` (rel32 @1) sin pre-patch -> reloc del driver.
    s.main_call_off = 1;
    s.has_import_call = false;
    s.ok = true;
    return s;
}

/**
 * @brief @c _start x86-32 para ELF freestanding: llama a main y termina
 *        via @c int @c 0x80 @c exit(1) con @c eax (regparm ret) como
 *        codigo.  Sin libc -- valido para kernels/modo protegido.
 *
 *   E8 rel32           call main             ; main justo despues del stub
 *   89 C3              mov  ebx, eax          ; codigo de salida en ebx
 *   B8 FC 00 00 00     mov  eax, 252          ; sys_exit_group (32-bit)
 *   CD 80              int  0x80
 *
 * NOTA: exit_group (252 en 32-bit), no exit (1), por la misma razon que en
 * x86-64: propaga el exit-code del proceso entero.
 */
StartStub start_x86_32_elf() {
    StartStub s;
    s.bytes = {
        0xE8, 0x00, 0x00, 0x00, 0x00, // call main (rel32@1)            (off 0)
        0x89, 0xC3,                   // mov ebx, eax                   (off 5)
        0xB8, 0xFC, 0x00, 0x00, 0x00, // mov eax, 252 (exit_group)      (off 7)
        0xCD, 0x80                    // int 0x80                       (off 12)
    };
    s.main_call_off = 1;
    s.has_import_call = false;
    s.ok = true;
    return s;
}

/**
 * @brief @c _start x86-32 para PE: llama a main y termina via
 *        @c kernel32!ExitProcess(eax) por la IAT (stdcall: push arg).
 *
 *   E8 rel32           call main             ; main (rel32 a parchear)
 *   50                 push eax               ; codigo de salida (stdcall)
 *   FF 15 disp32       call [ExitProcess]     ; IAT abs32 (a parchear)
 *   CC                 int3                   ; inalcanzable
 */
StartStub start_x86_32_pe() {
    StartStub s;
    s.bytes = {
        0xE8, 0x00, 0x00, 0x00, 0x00,       // call main (rel32@1)   (off 0)
        0x50,                               // push eax              (off 5)
        0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, // call [abs32] (off 6, disp@8)
        0xCC                                // int3                  (off 12)
    };
    s.main_call_off = 1;
    s.has_import_call = true;
    s.import_call_off = 6; // offset del FF 15
    s.import_dll = "kernel32.dll";
    s.import_func = "ExitProcess";
    s.ok = true;
    return s;
}

/**
 * @brief @c _start AArch64 para ELF bare-metal: llama a @c main y termina via
 *        SEMIHOSTING (@c SYS_EXIT) con @c w0 (retorno de main) como codigo.
 *        Modelo del tier bare: se ejecuta bajo @c qemu-system-aarch64
 *        @c -semihosting (cargado con @c -kernel), sin libc ni syscalls Linux.
 *
 *   d2a80614   movz x20, #0x4030, lsl #16  ; sp = 0x40300000 (RAM alta de virt)
 *   910202 9f  mov  sp, x20                ; el bare-metal NO trae sp inicializado
 *   97ffffff   bl main                     ; main return en x0 (imm26->reloc CALL26)
 *   aa0003f5   mov x21, x0                 ; guarda el codigo de salida
 *   d10043ff   sub sp, sp, #16
 *   d28004c2   movz x2, #0x26              ; ADP_Stopped_ApplicationExit = 0x20026
 *   f2a00042   movk x2, #0x2, lsl #16
 *   f90003e2   str x2, [sp]                ; parametro[0] = razon
 *   f90007f5   str x21, [sp, #8]           ; parametro[1] = exit code
 *   910003e1   mov x1, sp                  ; x1 -> bloque de parametros
 *   d2800300   movz x0, #0x18              ; SYS_EXIT (angel)
 *   d45e0000   hlt #0xf000                 ; trap de semihosting
 *
 * El `bl main` (offset 8) NO se pre-parchea: el driver declara una reloc
 * @c AOT_RELOC_ARM64_CALL26 a la VA real de @c main (imm26 = (main-site)>>2).
 * Fija @c sp a 0x40300000 (RAM de la machine @c virt de QEMU): el tier bare no
 * hereda pila de ningun cargador; para un ELF Linux-hosted el kernel ya la da.
 */
StartStub start_arm64_elf() {
    StartStub s;
    s.bytes = {
        0x14, 0x06, 0xa8, 0xd2, // movz x20, #0x4030, lsl #16       (off 0)
        0x9f, 0x02, 0x00, 0x91, // mov sp, x20                      (off 4)
        // Habilitar FP/SIMD: CPACR_EL1.FPEN = 0b11 (sin trap).  Sin esto, la
        // primera instruccion float (fmov/fadd/...) trapea al vector de
        // excepcion en bare-metal.
        0x00, 0x06, 0xa0, 0xd2, // movz x0, #0x30, lsl #16 (0x300000)(off 8)
        0x40, 0x10, 0x18, 0xd5, // msr cpacr_el1, x0                (off 12)
        0xdf, 0x3f, 0x03, 0xd5, // isb                             (off 16)
        0xfe, 0xff, 0xff, 0x97, // bl main (imm26@CALL26)           (off 20)
        0xf5, 0x03, 0x00, 0xaa, // mov x21, x0                      (off 12)
        0xff, 0x43, 0x00, 0xd1, // sub sp, sp, #16                  (off 16)
        0xc2, 0x04, 0x80, 0xd2, // movz x2, #0x26                   (off 20)
        0x42, 0x00, 0xa0, 0xf2, // movk x2, #0x2, lsl #16 (0x20026) (off 24)
        0xe2, 0x03, 0x00, 0xf9, // str x2, [sp]                     (off 28)
        0xf5, 0x07, 0x00, 0xf9, // str x21, [sp, #8]                (off 32)
        0xe1, 0x03, 0x00, 0x91, // mov x1, sp                       (off 36)
        0x00, 0x03, 0x80, 0xd2, // movz x0, #0x18 (SYS_EXIT)        (off 40)
        0x00, 0x00, 0x5e, 0xd4  // hlt #0xf000 (semihosting)        (off 44)
    };
    s.main_call_off = 20; // el `bl` esta en offset 20 (tras sp + enable FP)
    s.has_import_call = false;
    s.ok = true;
    return s;
}

} // namespace

StartStub aot_make_start_stub(AotArch arch, ObjFormat fmt) {
    switch (arch) {
    case AotArch::X86_64:
        if (fmt == ObjFormat::PE) return start_x86_64_pe();
        if (fmt == ObjFormat::ELF) return start_x86_64_elf();
        break;
    case AotArch::X86_32:
        if (fmt == ObjFormat::ELF) return start_x86_32_elf();
        if (fmt == ObjFormat::PE) return start_x86_32_pe();
        break;
    case AotArch::ARM64:
        if (fmt == ObjFormat::ELF) return start_arm64_elf();
        break;
    default: break;
    }
    StartStub s;
    s.ok = false;
    s.err = "combinacion (arch, formato) no soportada todavia en AOT "
            "(hoy: x86-64 PE/ELF, x86-32 PE/ELF, aarch64 ELF)";
    return s;
}

} // namespace aot
