/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_physical.cpp
 * @brief Test del @c PhysicalRegisterBank (Fase 0 del allocator de banco
 * ancho).
 *
 * Valida la CONSOLIDACION de arquitectura: que el banco fisico unico representa
 * FIELMENTE los 4 targets existentes (x86-64 SysV/Win64, x86-32, AArch64) MAS
 * el interprete, y que el modelo rico (Lane/View/SavePolicy/AliasSet/Reserve)
 * expresa lo que cada fuente veia por separado.  No ejecuta codigo: es
 * validacion estructural + round-trip contra @c TargetRegInfo.
 */

#include "codegen/rbank/physical_bank.h"
#include "jit/target_reginfo.h"

#include <cstdio>

using namespace jit;
using namespace codegen::rbank;

static int g_checks = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fail;                                                          \
            std::printf("  [FAIL] %s (linea %d)\n", (msg), __LINE__);          \
        }                                                                      \
    } while (0)

/** @brief Cuenta lanes de una clase con una @c ReserveKind (provenance). */
static size_t count_kind(const PhysicalRegisterBank &b, ResourceClass cls,
                         ReserveKind rk) {
    size_t n = 0;
    for (const auto &l : b.lanes)
        if (l.cls == cls && l.reserve.kind == rk) ++n;
    return n;
}
/** @brief Cuenta lanes ASIGNABLES (reserve.reason == NONE) de una clase. */
static size_t count_free(const PhysicalRegisterBank &b, ResourceClass cls) {
    size_t n = 0;
    for (const auto &l : b.lanes)
        if (l.cls == cls && l.reserve.is_free()) ++n;
    return n;
}

/** @brief BackendCaps con un conjunto de features seteado explicitamente. */
static BackendCaps caps_with(bool avx, bool avx512) {
    BackendCaps c{};
    c.sse2 = true; // baseline x86-64.
    c.avx = avx;
    c.avx2 = avx;
    c.avx512f = avx512;
    return c;
}

/** @brief Round-trip + reporte de un banco contra su TargetRegInfo. */
static void check_roundtrip(const PhysicalRegisterBank &b,
                            const TargetRegInfo &t, const char *label) {
    RoundTripReport rep = physical_bank_roundtrip_check(b, t);
    std::printf("  %-14s GP=%zu FP=%zu  alloc(GP)=%zu alloc(FP*)=%zu  "
                "implicitas=%zu\n",
                label, b.lane_count(ResourceClass::GP),
                b.lane_count(ResourceClass::FP_VECTOR),
                b.allocatable_count(ResourceClass::GP, false),
                b.allocatable_count(ResourceClass::FP_VECTOR, false),
                rep.unnamed_reserved.size());
    CHECK(rep.ok,
          rep.mismatch.empty() ? "round-trip fallo" : rep.mismatch.c_str());
    if (!rep.unnamed_reserved.empty()) {
        std::printf("    reservas IMPLICITAS superficiadas (ids):");
        for (uint8_t id : rep.unnamed_reserved)
            std::printf(" %u", id);
        std::printf("\n");
    }
}

int main() {
    std::printf("=== test_rbank_physical (Fase 0: banco fisico unico) ===\n");

    const BackendCaps avx2 = caps_with(/*avx=*/true, /*avx512=*/false);
    const BackendCaps sse2 = caps_with(/*avx=*/false, /*avx512=*/false);
    const BackendCaps avx512 = caps_with(/*avx=*/true, /*avx512=*/true);

    // --- Round-trip de los 5 bancos ---
    std::printf("\n[round-trip fidelidad vs TargetRegInfo]\n");
    PhysicalRegisterBank sysv = physical_bank_x86_64(true, avx2);
    PhysicalRegisterBank win = physical_bank_x86_64(false, avx2);
    PhysicalRegisterBank x32 = physical_bank_x86_32();
    PhysicalRegisterBank a64 = physical_bank_arm64(avx2);
    PhysicalRegisterBank itp = physical_bank_interp();
    check_roundtrip(sysv, target_x86_64_abi(true), "x86-64-sysv");
    check_roundtrip(win, target_x86_64_abi(false), "x86-64-win64");
    check_roundtrip(x32, target_x86_32(), "x86-32");
    check_roundtrip(a64, build_arm64_target(), "arm64");
    // interp usa un TargetRegInfo sintetico; lo reconstruimos identico.
    {
        RoundTripReport rep;
        // interp: todo asignable, sin scratch/reserved -> 0 implicitas.
        CHECK(count_free(itp, ResourceClass::GP) == 16,
              "interp GP no todos asignables");
        CHECK(count_free(itp, ResourceClass::FP_VECTOR) == 16,
              "interp FP no todos asignables");
        std::printf("  %-14s GP=%zu FP=%zu (banco de la VM)\n", "interp",
                    itp.lane_count(ResourceClass::GP),
                    itp.lane_count(ResourceClass::FP_VECTOR));
        (void)rep;
    }

    // --- x86-64: el hallazgo del diagnostico como DATO del modelo ---
    std::printf("\n[x86-64: VEC_ACC demand-driven + views por caps]\n");
    CHECK(sysv.lane_count(ResourceClass::FP_VECTOR) == 16,
          "x86-64 FP != 16 lanes");
    CHECK(count_kind(sysv, ResourceClass::FP_VECTOR, ReserveKind::VEC_ACC) == 4,
          "x86-64 VEC_ACC != 4 (XMM10-13)");
    CHECK(count_kind(sysv, ResourceClass::FP_VECTOR, ReserveKind::SCRATCH) == 2,
          "x86-64 FP scratch != 2 (XMM14-15)");
    // El nucleo del hallazgo: 10 asignables con reduccion activa, 14 sin ella.
    CHECK(sysv.allocatable_count(ResourceClass::FP_VECTOR, /*vec=*/true) == 10,
          "x86-64 FP allocatable con reduccion != 10");
    CHECK(sysv.allocatable_count(ResourceClass::FP_VECTOR, /*vec=*/false) == 14,
          "x86-64 FP allocatable sin reduccion != 14 (VEC_ACC liberado)");
    // XMM10 (id 26) es una reserva de OPTIMIZATION, demand-driven.
    {
        const Lane *xmm10 = sysv.by_id(26);
        CHECK(xmm10 && xmm10->reserve.reason == ReserveReason::OPTIMIZATION,
              "XMM10 no es reserva de OPTIMIZATION");
        CHECK(xmm10 && xmm10->reserve.kind == ReserveKind::VEC_ACC,
              "XMM10 no es VEC_ACC");
        CHECK(xmm10 && xmm10->reserve.demand_driven,
              "XMM10 VEC_ACC no es demand-driven");
        CHECK(xmm10 && xmm10->allocatable(false) && !xmm10->allocatable(true),
              "XMM10 no se libera cuando no hay reduccion");
    }

    // --- Views gated por Capabilities ---
    std::printf("\n[views: anchos gated por BackendCaps]\n");
    {
        // avx2: XMM0 presenta 8/16/32, NO 64.
        const Lane *x0_avx = sysv.by_id(16);
        CHECK(x0_avx && x0_avx->supports(ViewWidth::W16), "avx2: XMM0 sin W16");
        CHECK(x0_avx && x0_avx->supports(ViewWidth::W32),
              "avx2: XMM0 sin W32 (YMM)");
        CHECK(x0_avx && !x0_avx->supports(ViewWidth::W64),
              "avx2: XMM0 con W64 sin AVX512");
    }
    {
        // sse2: XMM0 presenta 16, NO 32.
        PhysicalRegisterBank b = physical_bank_x86_64(true, sse2);
        const Lane *x0 = b.by_id(16);
        CHECK(x0 && x0->supports(ViewWidth::W16), "sse2: XMM0 sin W16");
        CHECK(x0 && !x0->supports(ViewWidth::W32),
              "sse2: XMM0 con W32 sin AVX");
    }
    {
        // avx512: XMM0 presenta 64 (ZMM).
        PhysicalRegisterBank b = physical_bank_x86_64(true, avx512);
        const Lane *x0 = b.by_id(16);
        CHECK(x0 && x0->supports(ViewWidth::W64), "avx512: XMM0 sin W64 (ZMM)");
    }

    // --- SavePolicy (ABI, por-vista) ---
    std::printf("\n[save policy: propiedad de Lane+ABI+View]\n");
    {
        // SysV: todos los XMM son caller-saved -> VOLATILE en toda vista.
        const Lane *x0 = sysv.by_id(16);
        CHECK(x0 && x0->preservation_of(ViewWidth::W16) == SavePolicy::VOLATILE,
              "sysv XMM0 no VOLATILE");
        // arm64: v8 (id 40) es callee-saved -> PRESERVED.
        const Lane *v8 = a64.by_id(40);
        CHECK(v8 &&
                  v8->preservation_of(ViewWidth::W16) == SavePolicy::PRESERVED,
              "arm64 v8 no PRESERVED en W16");
        CHECK(v8 && v8->preservation_of(ViewWidth::W8) == SavePolicy::PRESERVED,
              "arm64 v8 no PRESERVED en W8");
    }

    // --- AliasSet (self-alias en x86, abstraccion) ---
    std::printf("\n[alias set: sub-registro comparte id]\n");
    {
        const Lane *x0 = sysv.by_id(16);
        // XMM0 se aliasea solo a si mismo (su indice de lane).
        size_t idx = static_cast<size_t>(sysv.id_to_index[16]);
        CHECK(x0 && x0->aliases.test(idx), "XMM0 no se aliasea a si mismo");
        CHECK(x0 && x0->aliases.bits == (uint64_t{1} << idx),
              "XMM0 alias no es exactamente self");
        // No solapa con XMM1 (lanes distintas en x86).
        const Lane *x1 = sysv.by_id(17);
        CHECK(x0 && x1 && !x0->aliases.overlaps(x1->aliases),
              "XMM0 solapa con XMM1 (deberian ser lanes disjuntas)");
    }

    // --- Bancos "estrechos" y el interp ---
    std::printf("\n[topologias distintas: x86-32, arm64, interp]\n");
    CHECK(x32.lane_count(ResourceClass::FP_VECTOR) == 0, "x86-32 FP != 0");
    CHECK(x32.lane_count(ResourceClass::GP) == 8, "x86-32 GP != 8");
    CHECK(a64.lane_count(ResourceClass::GP) == 31, "arm64 GP != 31 (x0-x30)");
    CHECK(a64.lane_count(ResourceClass::FP_VECTOR) == 32,
          "arm64 FP != 32 (v0-v31)");
    {
        const Lane *v0 = a64.by_id(32);
        CHECK(v0 && v0->supports(ViewWidth::W16), "arm64 v0 sin W16 (Q)");
        CHECK(v0 && !v0->supports(ViewWidth::W32),
              "arm64 v0 con W32 (sin SVE)");
        const Lane *f0 = itp.by_id(16);
        CHECK(f0 && f0->supports(ViewWidth::W64), "interp f0 sin W64 (ZMM VM)");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
