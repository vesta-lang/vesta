/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file native_backend.cpp
 * @brief Implementacion de los backends de codegen nativo (x86 y arm64).
 */

#include "toolchain/native_backend.h"

#include "jit/arm64/arm64_select.h"
#include "vx/asm/asm_backend.h"

namespace aot {

namespace {

/// Backend x86: envuelve @c jit::vreg_compile_native (comportamiento identico al
/// del driver antes del desacoplamiento).
class X86Backend : public NativeBackend {
  public:
    explicit X86Backend(AotArch a) : arch_(a) {}
    AotArch arch() const override { return arch_; }
    const char *name() const override { return "x86"; }
    NativeCompileResult
    compile_function(const ir::IrFunction &fn,
                     const NativeCompileOpts &opts) override {
        NativeCompileResult r;
        r.bytes = jit::vreg_compile_native(
            fn, {}, {}, {}, {}, &r.relocs, opts.pic, opts.target_sysv,
            opts.mode32, opts.fisa, /*emit_line_map=*/false,
            /*line_map_out=*/nullptr, /*asm_labels_out=*/nullptr,
            /*stackmaps_out=*/&r.stackmaps);
        return r;
    }

  private:
    AotArch arch_;
};

/// Backend arm64: baja la funcion a texto AArch64 (arm64_emit_asm) y lo ensambla
/// con Keystone.  Subconjunto entero (incluye ramas, llamadas intra-unit y
/// atomicos); las funciones con ops no soportadas devuelven bytes vacios.
class Arm64Backend : public NativeBackend {
  public:
    AotArch arch() const override { return AotArch::ARM64; }
    const char *name() const override { return "arm64"; }
    NativeCompileResult
    compile_function(const ir::IrFunction &fn,
                     const NativeCompileOpts &opts) override {
        (void)opts;
        NativeCompileResult r;
        bool unsupported = false;
        const std::string text = jit::arm64::arm64_emit_asm(fn, unsupported);
        if (unsupported || text.empty())
            return r; // no soportada.
        if (!vx::g_asm_backend)
            return r; // sin ensamblador registrado.
        vx::AsmAssembleResult asmres =
            vx::g_asm_backend->assemble(text, vx::AsmArch::ARM64);
        if (!asmres.ok)
            return r; // sintaxis no ensamblable -> tratar como no soportada.
        r.bytes = std::move(asmres.bytes);
        // Relocs cross-funcion arm64 (R_AARCH64_CALL26) -> H.5b.  Por ahora el
        // subconjunto sin CALL cross-modulo no genera relocs.
        return r;
    }
};

} // namespace

std::unique_ptr<NativeBackend> make_native_backend(AotArch arch) {
    switch (arch) {
    case AotArch::X86_64:
    case AotArch::X86_32:
    case AotArch::X86_16:
        return std::unique_ptr<NativeBackend>(new X86Backend(arch));
    case AotArch::ARM64:
        return std::unique_ptr<NativeBackend>(new Arm64Backend());
    default:
        return nullptr;
    }
}

} // namespace aot
