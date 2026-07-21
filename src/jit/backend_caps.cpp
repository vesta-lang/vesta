/**
 * @file backend_caps.cpp
 * @brief Implementacion de la deteccion de capacidades del backend (ver
 *        backend_caps.h).  Deteccion CPUID centralizada (host) + capa de la DB
 *        de instrucciones (@c --cpu) + fallback coarse (@c --float-isa).
 */

#include "jit/backend_caps.h"

#include "vx/asm/instr_db.h"

#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h> // __get_cpuid / __get_cpuid_count
#endif

namespace jit {

// ------------------------------------------------------------------------
//  from_bits / to_bits
// ------------------------------------------------------------------------

BackendCaps BackendCaps::from_bits(uint64_t b) {
    BackendCaps c;
    c.sse2 = (b & CF_SSE2) != 0;
    c.sse42 = (b & CF_SSE42) != 0;
    c.popcnt = (b & CF_POPCNT) != 0;
    c.avx = (b & CF_AVX) != 0;
    c.avx2 = (b & CF_AVX2) != 0;
    c.bmi1 = (b & CF_BMI1) != 0;
    c.bmi2 = (b & CF_BMI2) != 0;
    c.avx512f = (b & CF_AVX512F) != 0;
    c.erms = (b & CF_ERMS) != 0;
    c.fma = (b & CF_FMA) != 0;
    c.lzcnt = (b & CF_LZCNT) != 0;
    c.f16c = (b & CF_F16C) != 0;
    c.sha = (b & CF_SHA) != 0;
    c.aes = (b & CF_AES) != 0;
    return c;
}

uint64_t BackendCaps::to_bits() const {
    uint64_t b = 0;
    if (sse2)
        b |= CF_SSE2;
    if (sse42)
        b |= CF_SSE42;
    if (popcnt)
        b |= CF_POPCNT;
    if (avx)
        b |= CF_AVX;
    if (avx2)
        b |= CF_AVX2;
    if (bmi1)
        b |= CF_BMI1;
    if (bmi2)
        b |= CF_BMI2;
    if (avx512f)
        b |= CF_AVX512F;
    if (erms)
        b |= CF_ERMS;
    if (fma)
        b |= CF_FMA;
    if (lzcnt)
        b |= CF_LZCNT;
    if (f16c)
        b |= CF_F16C;
    if (sha)
        b |= CF_SHA;
    if (aes)
        b |= CF_AES;
    return b;
}

// ------------------------------------------------------------------------
//  Deteccion del HOST via CPUID (+ XGETBV para el estado AVX habilitado por SO)
// ------------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
// Lee XCR0 (subleaf 0).  Solo legal si OSXSAVE esta activo (CPUID.1:ECX.27).
static uint64_t read_xcr0() {
    uint32_t eax = 0, edx = 0;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<uint64_t>(edx) << 32) | eax;
}
#endif

uint64_t backend_caps_host_bits() {
    static uint64_t cached = 0;
    static bool done = false;
    if (done)
        return cached;
    uint64_t f = 0;
#if defined(__GNUC__) || defined(__clang__)
    unsigned a = 0, b = 0, c = 0, d = 0;
    bool osxsave = false, avx_os = false, avx512_os = false;
    if (__get_cpuid(1u, &a, &b, &c, &d)) {
        if (d & (1u << 26))
            f |= CF_SSE2; // EDX.26 SSE2
        if (c & (1u << 20))
            f |= CF_SSE42; // ECX.20 SSE4.2
        if (c & (1u << 23))
            f |= CF_POPCNT; // ECX.23 POPCNT
        if (c & (1u << 25))
            f |= CF_AES; // ECX.25 AES-NI
        if (c & (1u << 29))
            f |= CF_F16C;         // ECX.29 F16C
        osxsave = (c & (1u << 27)) != 0; // ECX.27 OSXSAVE
        // AVX (ECX.28) y FMA (ECX.12) requieren estado YMM habilitado por el SO.
        const bool avx_cpu = (c & (1u << 28)) != 0;
        const bool fma_cpu = (c & (1u << 12)) != 0;
        if (osxsave) {
            const uint64_t xcr0 = read_xcr0();
            avx_os = (xcr0 & 0x6) == 0x6;         // XMM(1)+YMM(2)
            avx512_os = (xcr0 & 0xE6) == 0xE6;    // + opmask(5)+ZMM_hi(6)+ZMM16(7)
        }
        if (avx_cpu && avx_os)
            f |= CF_AVX;
        if (fma_cpu && avx_os)
            f |= CF_FMA; // FMA usa el banco YMM -> requiere avx_os
    }
    if (__get_cpuid_count(7u, 0u, &a, &b, &c, &d)) {
        if (b & (1u << 3))
            f |= CF_BMI1; // EBX.3 BMI1 (incl. TZCNT)
        if (b & (1u << 8))
            f |= CF_BMI2; // EBX.8 BMI2 (PDEP/PEXT)
        if (b & (1u << 9))
            f |= CF_ERMS; // EBX.9 ERMS
        if (b & (1u << 29))
            f |= CF_SHA; // EBX.29 SHA
        if (avx_os && (b & (1u << 5)))
            f |= CF_AVX2; // EBX.5 AVX2 (banco YMM)
        if (avx512_os && (b & (1u << 16)))
            f |= CF_AVX512F; // EBX.16 AVX-512F (banco ZMM)
    }
    // LZCNT / ABM: leaf extendido 0x80000001, ECX.5.
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) {
        if (c & (1u << 5))
            f |= CF_LZCNT;
    }
#else
    // Sin cpuid.h: x86-64 garantiza SSE2 por ABI base.
    f = CF_SSE2;
#endif
    cached = f;
    done = true;
    return f;
}

BackendCaps backend_caps_host() {
    return BackendCaps::from_bits(backend_caps_host_bits());
}

// ------------------------------------------------------------------------
//  Caps desde la DB de instrucciones (--cpu <microarch>)
// ------------------------------------------------------------------------

BackendCaps backend_caps_from_cpu(const std::string &cpu_name) {
    BackendCaps c; // baseline: solo sse2
    if (cpu_name.empty() || cpu_name == "generic")
        return c;
    const int32_t id =
        vx::instr_db::cpu_by_name(vx::instr_db::Isa::X86, cpu_name);
    if (id < 0)
        return c; // microarq desconocida -> baseline seguro
    const uint32_t cid = static_cast<uint32_t>(id);
    auto has = [&](const char *feat) {
        return vx::instr_db::cpu_has_feature(vx::instr_db::Isa::X86, cid, feat);
    };
    c.sse2 = true; // x86-64 base
    c.sse42 = has("SSE42");
    c.popcnt = has("POPCNT");
    c.avx = has("AVX");
    c.avx2 = has("AVX2");
    c.bmi1 = has("BMI1");
    c.bmi2 = has("BMI2");
    c.avx512f = has("AVX512");
    c.fma = has("FMA");
    c.lzcnt = has("LZCNT");
    c.f16c = has("F16C");
    c.sha = has("SHA");
    c.aes = has("AES");
    return c;
}

// ------------------------------------------------------------------------
//  Caps coarse desde --float-isa (fallback conservador)
// ------------------------------------------------------------------------

BackendCaps backend_caps_from_float_isa(FloatIsa fisa) {
    BackendCaps c; // baseline sse2
    switch (fisa) {
    case FloatIsa::AVX512F:
        // Nivel x86-64-v4: garantiza AVX2+FMA+BMI2+AVX-512.
        c.avx512f = true;
        c.avx2 = true;
        c.avx = true;
        c.fma = true;
        c.bmi2 = true;
        c.bmi1 = true;
        c.popcnt = true;
        c.sse42 = true;
        c.lzcnt = true;
        break;
    case FloatIsa::AVX:
        // El nivel `avx` de --float-isa = AVX2 256b (ver la ayuda).  AVX2 implica
        // FMA3 en la practica (Haswell+; no hay CPU con AVX2 sin FMA3), asi que
        // el target AVX2 SI puede emitir VFMADD.
        c.avx = true;
        c.avx2 = true;
        c.fma = true;
        c.sse42 = true;
        c.popcnt = true;
        break;
    case FloatIsa::SSE2:
    case FloatIsa::X87:
    case FloatIsa::AUTO:
    default:
        break; // solo sse2
    }
    return c;
}

// ------------------------------------------------------------------------
//  Resolver central
// ------------------------------------------------------------------------

BackendCaps resolve_backend_caps(const std::string &cpu, bool jit_host,
                                 FloatIsa fisa) {
    if (!cpu.empty() && cpu != "generic")
        return backend_caps_from_cpu(cpu); // 1. --cpu (DB)
    if (jit_host)
        return backend_caps_host(); // 2. host (JIT)
    return backend_caps_from_float_isa(fisa); // 3. coarse
}

} // namespace jit
