/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/vec_isa.h
 * @brief Seleccion del ancho SIMD para la auto-vectorizacion (SSE2/AVX2/AVX512).
 *
 * Dos decisiones ORTOGONALES:
 *
 *  1. @c vec_chunk_isa() -- lo usa el MATCHER (vectorize.cpp) para elegir el
 *     "chunk width" del bucle (16/32/64 bytes = stride W de 2/4/8 f64).  Es la
 *     granularidad con la que el bucle avanza; un chunk mas grande = menos
 *     iteraciones (menos overhead de branch) pero mas cola escalar (N % W).
 *     El IR resultante es PORTABLE: el interprete lo baja escalar por lane y el
 *     JIT lo descompone al ancho del host (ver punto 2).
 *
 *  2. @c vec_emit_isa() -- lo usa el JIT (vreg_select.cpp) para elegir el ancho
 *     de las instrucciones SIMD emitidas.  Un chunk de 64 bytes se emite como
 *     1xZMM (AVX512), 2xYMM (AVX2) o 4xXMM (SSE2) segun el host.  Asi el codigo
 *     nativo SIEMPRE corre en la maquina actual sea cual sea el chunk horneado.
 *
 * Ambos se autodetectan por cpuid (@c __builtin_cpu_supports) y se pueden
 * forzar por variable de entorno:
 *   - @c VESTA_VEC_ISA       = sse2|avx2|avx512|auto  (chunk del matcher)
 *   - @c VESTA_JIT_VEC_ISA   = sse2|avx2|avx512|auto  (emision del JIT)
 * El override de emision permite VALIDAR el codegen AVX512 por DISASSEMBLY en
 * una CPU sin AVX512 (forzar avx512 + VESTA_JIT_DISASM=1, sin ejecutar).
 */

#ifndef VESTA_JIT_VEC_ISA_H
#define VESTA_JIT_VEC_ISA_H

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace jit {

/** @brief Nivel SIMD para la vectorizacion. */
enum class VecIsa : uint8_t {
    SSE2 = 0,   ///< 128-bit (XMM): 2x f64 / 4x i32.  Baseline x86-64.
    AVX2 = 1,   ///< 256-bit (YMM): 4x f64 / 8x i32.
    AVX512 = 2, ///< 512-bit (ZMM): 8x f64 / 16x i32.
};

/** @brief Ancho en bytes del vector de un @c VecIsa (16/32/64). */
inline uint32_t vec_isa_width(VecIsa i) noexcept {
    return (i == VecIsa::AVX512) ? 64u : (i == VecIsa::AVX2) ? 32u : 16u;
}

/** @brief Autodeteccion por cpuid del nivel SIMD del host. */
inline VecIsa vec_isa_host() noexcept {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f")) return VecIsa::AVX512;
    if (__builtin_cpu_supports("avx2")) return VecIsa::AVX2;
    return VecIsa::SSE2; // x86-64 garantiza SSE2
}

/** @brief Parsea un override de entorno; devuelve @p deflt si ausente/invalido. */
inline VecIsa vec_isa_from_env(const char *var, VecIsa deflt) noexcept {
    const char *v = std::getenv(var);
    if (!v || !*v) return deflt;
    if (std::strcmp(v, "sse2") == 0) return VecIsa::SSE2;
    if (std::strcmp(v, "avx2") == 0) return VecIsa::AVX2;
    if (std::strcmp(v, "avx512") == 0) return VecIsa::AVX512;
    if (std::strcmp(v, "auto") == 0) return deflt;
    return deflt;
}

/** @brief ISA del CHUNK que el matcher hornea (VESTA_VEC_ISA, default host). */
inline VecIsa vec_chunk_isa() noexcept {
    static const VecIsa v =
        vec_isa_from_env("VESTA_VEC_ISA", vec_isa_host());
    return v;
}

/** @brief ISA de EMISION del JIT (VESTA_JIT_VEC_ISA, default host).  El
 *  override permite forzar emision avx512 para validar por disasm en una CPU
 *  sin avx512 (no ejecutar: seria SIGILL). */
inline VecIsa vec_emit_isa() noexcept {
    static const VecIsa v =
        vec_isa_from_env("VESTA_JIT_VEC_ISA", vec_isa_host());
    return v;
}

} // namespace jit

#endif // VESTA_JIT_VEC_ISA_H
