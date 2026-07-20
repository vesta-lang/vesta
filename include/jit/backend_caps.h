/**
 * @file backend_caps.h
 * @brief Capacidades del backend de codegen: que extensiones de ISA puede
 *        EMITIR el JIT/AOT (FMA, BMI2, AVX2, AVX-512, POPCNT, LZCNT, ...).
 *
 * Centraliza la deteccion de features en un solo sitio (evita comprobaciones
 * CPUID desperdigadas por el backend).  El lowering consulta estas caps:
 *   @code
 *   if (caps.fma) emit(VFMADD231SD); else { emit(MULSD); emit(ADDSD); }
 *   @endcode
 *
 * Tres FUENTES (por orden de prioridad en @c resolve_backend_caps):
 *   1. @c --cpu <microarch> : desde la DB de instrucciones (@c cpu_has_feature)
 *      -- reproducible en cross-compile, no depende del host.
 *   2. JIT (host)           : CPUID del host (la VM corre sobre la CPU real).
 *   3. @c --float-isa       : nivel float coarse (fallback conservador).
 */
#ifndef VESTA_JIT_BACKEND_CAPS_H
#define VESTA_JIT_BACKEND_CAPS_H

#include <cstdint>
#include <string>

#include "jit/vreg_select.h" // FloatIsa

namespace jit {

/**
 * @brief Bits de features del HOST -- layout COMPARTIDO con
 *        @c vesta_runtime_cpu_features() y el @c __vx_cpu_init del AOT.
 *        Los bits 0..8 son historicos (no reordenar); 9+ anadidos aqui.
 */
enum CpuFeatureBit : uint64_t {
    CF_SSE2 = 1ull << 0,
    CF_SSE42 = 1ull << 1,
    CF_POPCNT = 1ull << 2,
    CF_AVX = 1ull << 3,
    CF_AVX2 = 1ull << 4,
    CF_BMI1 = 1ull << 5,
    CF_BMI2 = 1ull << 6,
    CF_AVX512F = 1ull << 7,
    CF_ERMS = 1ull << 8,
    CF_FMA = 1ull << 9,
    CF_LZCNT = 1ull << 10,
    CF_F16C = 1ull << 11,
    CF_SHA = 1ull << 12,
    CF_AES = 1ull << 13,
};

/**
 * @brief Que instrucciones puede emitir el backend.  @c sse2 es baseline en
 *        x86-64 (siempre true); el resto se pobla desde la fuente elegida.
 */
struct BackendCaps {
    bool sse2 = true;
    bool sse42 = false;
    bool popcnt = false;
    bool avx = false;
    bool avx2 = false;
    bool bmi1 = false;
    bool bmi2 = false;
    bool avx512f = false;
    bool erms = false;
    bool fma = false; ///< FMA3 (VFMADD*).  OJO: AVX2 NO lo implica.
    bool lzcnt = false;
    bool f16c = false;
    bool sha = false;
    bool aes = false;

    /// Decodifica un bitmask @c CpuFeatureBit a caps.
    static BackendCaps from_bits(uint64_t b);
    /// Codifica las caps al bitmask @c CpuFeatureBit.
    uint64_t to_bits() const;
};

/**
 * @brief Caps del HOST via CPUID (leaf 1 + leaf 7 + XGETBV para el estado AVX).
 *        Cacheado (cpuid corre una vez).  Para el JIT.
 */
BackendCaps backend_caps_host();

/**
 * @brief Bitmask de features del host (mismo layout @c CpuFeatureBit).  Fuente
 *        unica de la deteccion; @c vesta_runtime_cpu_features la reusa.
 */
uint64_t backend_caps_host_bits();

/**
 * @brief Caps de una microarquitectura @p cpu_name desde la DB de instrucciones
 *        (@c idb::cpu_has_feature).  Para @c --cpu.  Si la microarq no existe en
 *        la DB, devuelve caps baseline (solo sse2).
 */
BackendCaps backend_caps_from_cpu(const std::string &cpu_name);

/**
 * @brief Caps coarse desde el nivel @c --float-isa.  Conservador: solo lo que
 *        el nivel GARANTIZA (AVX no implica FMA -> fma=false salvo AVX512F).
 */
BackendCaps backend_caps_from_float_isa(FloatIsa fisa);

/**
 * @brief Resolver central.  Prioridad: @p cpu (DB) > host CPUID (si @p jit_host)
 *        > @p fisa (coarse).  Un solo punto de decision para todo el backend.
 */
BackendCaps resolve_backend_caps(const std::string &cpu, bool jit_host,
                                 FloatIsa fisa);

} // namespace jit

#endif // VESTA_JIT_BACKEND_CAPS_H
