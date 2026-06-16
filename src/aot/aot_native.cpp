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
                0x48, 0x83, 0xEC, 0x28,              // sub rsp, 0x28          (off 0)
                0xE8, 0x00, 0x00, 0x00, 0x00,        // call main (rel32@5)    (off 4)
                0x89, 0xC1,                          // mov ecx, eax           (off 9)
                0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,  // call [rip+disp] (off 11, disp@13)
                0xCC                                 // int3                   (off 17)
            };
            // AOT 2b: el `call main` (rel32 @5) NO se pre-parchea -> el driver
            // declara una reloc REL32 a la VA real de main (puede vivir en
            // cualquier seccion).  rel32 queda en 0 hasta que el writer resuelve.
            s.main_call_off = 5;
            s.has_import_call = true;
            s.import_call_off = 11;            // offset del FF 15
            s.import_dll  = "KERNEL32.dll";
            s.import_func = "ExitProcess";
            s.ok = true;
            return s;
        }

        /**
         * @brief @c _start x86-64 para ELF freestanding: llama a main y termina
         *        via @c syscall @c exit(60) con @c eax como codigo.  Sin libc ni
         *        imports -- valido para kernels/bootloaders.
         *
         *   E8 rel32           call main             ; main justo despues del stub
         *   89 C7              mov  edi, eax          ; arg0 = codigo de salida
         *   B8 3C 00 00 00     mov  eax, 60           ; sys_exit
         *   0F 05              syscall
         */
        StartStub start_x86_64_elf() {
            StartStub s;
            s.bytes = {
                0xE8, 0x00, 0x00, 0x00, 0x00,        // call main (rel32@1)    (off 0)
                0x89, 0xC7,                          // mov edi, eax           (off 5)
                0xB8, 0x3C, 0x00, 0x00, 0x00,        // mov eax, 60 (sys_exit) (off 7)
                0x0F, 0x05                           // syscall                (off 12)
            };
            // AOT 2b: `call main` (rel32 @1) sin pre-patch -> reloc del driver.
            s.main_call_off = 1;
            s.has_import_call = false;
            s.ok = true;
            return s;
        }

    } // namespace

    StartStub aot_make_start_stub(AotArch arch, ObjFormat fmt) {
        switch (arch) {
            case AotArch::X86_64:
                if (fmt == ObjFormat::PE)  return start_x86_64_pe();
                if (fmt == ObjFormat::ELF) return start_x86_64_elf();
                break;
            default:
                break;
        }
        StartStub s;
        s.ok  = false;
        s.err = "combinacion (arch, formato) no soportada todavia en AOT "
                "(hoy: x86-64 PE/ELF)";
        return s;
    }

} // namespace aot
