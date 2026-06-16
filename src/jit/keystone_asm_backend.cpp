/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file keystone_asm_backend.cpp
 * @brief Phase AS inc.4b: impl de @c vex::AsmBackend con Keystone.
 *
 * UNICO fichero del proyecto que incluye @c keystone.h para Phase AS (la
 * decision de diseno exige aislar la dependencia tras la interfaz pura
 * @c vex::AsmBackend).  Ensambla texto NASM Intel a bytes; usado hoy para
 * validar la sintaxis del body en compile-time (inc.4b) y, en inc.5, para
 * producir los bytes que van al code-cache del JIT.
 */

#include "jit/keystone_asm_backend.h"
#include "vex/asm_backend.h"

#include <keystone/keystone.h>

#include <mutex>

namespace jit {

    namespace {
        /// Traduce @c vex::AsmArch a (ks_arch, mode) de Keystone.
        bool arch_to_ks(vex::AsmArch a, ks_arch &arch, ks_mode &mode) {
            switch (a) {
                case vex::AsmArch::X86_64: arch = KS_ARCH_X86; mode = KS_MODE_64; return true;
                case vex::AsmArch::X86_32: arch = KS_ARCH_X86; mode = KS_MODE_32; return true;
                case vex::AsmArch::X86_16: arch = KS_ARCH_X86; mode = KS_MODE_16; return true;
                case vex::AsmArch::ARM64:  arch = KS_ARCH_ARM64; mode = KS_MODE_LITTLE_ENDIAN; return true;
                case vex::AsmArch::ARM32:  arch = KS_ARCH_ARM; mode = KS_MODE_ARM; return true;
            }
            return false;
        }

        /// Impl concreta: abre Keystone por cada @c assemble (stateless y
        /// thread-safe; el coste de @c ks_open es despreciable frente al
        /// compile-time global).
        struct KeystoneAsmBackend final : vex::AsmBackend {
            vex::AsmAssembleResult assemble(const std::string &nasm,
                                            vex::AsmArch arch) override {
                vex::AsmAssembleResult r;
                ks_arch ka; ks_mode km;
                if (!arch_to_ks(arch, ka, km)) {
                    r.error = "arquitectura no soportada por el backend Keystone";
                    return r;
                }
                ks_engine *ks = nullptr;
                if (ks_open(ka, km, &ks) != KS_ERR_OK || ks == nullptr) {
                    r.error = "ks_open fallo";
                    return r;
                }
                // El cuerpo es NASM Intel (copy-paste de docs Intel).
                ks_option(ks, KS_OPT_SYNTAX, KS_OPT_SYNTAX_NASM);

                unsigned char *enc = nullptr;
                size_t enc_size = 0, stat_count = 0;
                const int rc = ks_asm(ks, nasm.c_str(), 0,
                                      &enc, &enc_size, &stat_count);
                if (rc != 0) {
                    const ks_err e = ks_errno(ks);
                    r.ok = false;
                    r.error = ks_strerror(e);
                } else {
                    r.ok = true;
                    r.bytes.assign(enc, enc + enc_size);
                }
                if (enc) ks_free(enc);
                ks_close(ks);
                return r;
            }
        };
    } // namespace

    void register_keystone_asm_backend() {
        static std::once_flag once;
        static KeystoneAsmBackend backend;
        std::call_once(once, [] {
            vex::g_asm_backend = &backend;
        });
    }

} // namespace jit
