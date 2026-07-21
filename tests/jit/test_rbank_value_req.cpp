/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_value_req.cpp
 * @brief Test de @c ValueRequirements (nivel 1 del modelo de banco ancho): el
 *        link entre "que necesita un valor" y "que ofrece el banco fisico" via
 *        satisfacibilidad.  Verifica la derivacion pura (bytes->ancho, tipo->
 *        clase) y que la satisfacibilidad devuelve DATOS i18n-ready.
 */

#include "jit/rbank/physical_bank.h"
#include "jit/rbank/value_requirements.h"

#include <cstdio>

using namespace jit;
using namespace jit::rbank;

static int g_checks = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_fail;                                                        \
            std::printf("  [FAIL] %s (linea %d)\n", (msg), __LINE__);        \
        }                                                                    \
    } while (0)

/** @brief Construye un requisito escalar simple. */
static ValueRequirements req(ResourceClass cls, ViewWidth w,
                             Residency res = Residency::ANY,
                             int16_t fixed = -1) {
    ValueRequirements r;
    r.cls = cls;
    r.width = w;
    r.residency = res;
    r.fixed_reg = fixed;
    return r;
}

int main() {
    std::printf("=== test_rbank_value_req (Fase 0: ValueRequirements) ===\n");

    const BackendCaps avx2   = [] { BackendCaps c{}; c.sse2 = c.avx = c.avx2 = true; return c; }();
    const BackendCaps sse2   = [] { BackendCaps c{}; c.sse2 = true; return c; }();
    const BackendCaps avx512 = [] { BackendCaps c{}; c.sse2 = c.avx = c.avx2 = c.avx512f = true; return c; }();

    PhysicalRegisterBank b_avx2   = physical_bank_x86_64(true, avx2);
    PhysicalRegisterBank b_sse2   = physical_bank_x86_64(true, sse2);
    PhysicalRegisterBank b_avx512 = physical_bank_x86_64(true, avx512);
    PhysicalRegisterBank b_x32    = physical_bank_x86_32();

    // --- Derivacion pura ---
    std::printf("\n[derivacion pura tipo -> clase/ancho]\n");
    CHECK(view_width_for_bytes(1) == ViewWidth::W1, "1B != W1");
    CHECK(view_width_for_bytes(3) == ViewWidth::W4, "3B redondea != W4");
    CHECK(view_width_for_bytes(8) == ViewWidth::W8, "8B != W8");
    CHECK(view_width_for_bytes(12) == ViewWidth::W16, "12B redondea != W16");
    CHECK(view_width_for_bytes(32) == ViewWidth::W32, "32B != W32");
    CHECK(view_width_for_bytes(64) == ViewWidth::W64, "64B != W64");
    CHECK(resource_class_for(true) == ResourceClass::FP_VECTOR, "float != FP_VECTOR");
    CHECK(resource_class_for(false) == ResourceClass::GP, "int != GP");

    auto sat = [](const SatisfiabilityReport &r) { return r.ok; };
    auto why = [](const SatisfiabilityReport &r) { return r.reason; };

    // --- Casos generales (sin pin) ---
    std::printf("\n[satisfacibilidad general]\n");
    CHECK(sat(requirements_satisfiable(req(ResourceClass::GP, ViewWidth::W8), b_avx2, false)),
          "GP W8 no satisfacible en x86-64");
    CHECK(sat(requirements_satisfiable(req(ResourceClass::FP_VECTOR, ViewWidth::W8), b_avx2, false)),
          "FP W8 no satisfacible en x86-64");
    // FP W64 (ZMM): OK con avx512, NO con avx2/sse2.
    CHECK(sat(requirements_satisfiable(req(ResourceClass::FP_VECTOR, ViewWidth::W64), b_avx512, false)),
          "FP W64 no satisfacible con avx512");
    {
        auto rep = requirements_satisfiable(req(ResourceClass::FP_VECTOR, ViewWidth::W64), b_avx2, false);
        CHECK(!sat(rep) && why(rep) == UnsatReason::WIDTH_UNSUPPORTED,
              "FP W64 en avx2 no da WIDTH_UNSUPPORTED");
    }
    {
        auto rep = requirements_satisfiable(req(ResourceClass::FP_VECTOR, ViewWidth::W32), b_sse2, false);
        CHECK(!sat(rep) && why(rep) == UnsatReason::WIDTH_UNSUPPORTED,
              "FP W32 (YMM) en sse2 no da WIDTH_UNSUPPORTED");
    }
    {
        // x86-32 no tiene banco FP -> NO_LANE_OF_CLASS.
        auto rep = requirements_satisfiable(req(ResourceClass::FP_VECTOR, ViewWidth::W8), b_x32, false);
        CHECK(!sat(rep) && why(rep) == UnsatReason::NO_LANE_OF_CLASS,
              "FP en x86-32 no da NO_LANE_OF_CLASS");
    }

    // --- Residencia en memoria: siempre satisfacible ---
    std::printf("\n[residencia en memoria]\n");
    CHECK(sat(requirements_satisfiable(
              req(ResourceClass::GP, ViewWidth::W8, Residency::MEMORY), b_x32, false)),
          "MEMORY no satisfacible");
    {
        ValueRequirements r = req(ResourceClass::FP_VECTOR, ViewWidth::W8);
        r.address_taken = true; // implica must_be_memory
        CHECK(sat(requirements_satisfiable(r, b_x32, false)),
              "address_taken (memoria) no satisfacible aun sin banco FP");
    }

    // --- Pines duros (fixed_reg) ---
    std::printf("\n[pines duros fixed_reg -> DATO i18n]\n");
    // XMM0 (id 16) asignable, FP W8 -> OK.
    CHECK(sat(requirements_satisfiable(
              req(ResourceClass::FP_VECTOR, ViewWidth::W8, Residency::ANY, 16), b_avx2, false)),
          "pin XMM0 FP W8 no OK");
    // XMM0 pero clase GP -> WRONG_CLASS.
    {
        auto rep = requirements_satisfiable(
            req(ResourceClass::GP, ViewWidth::W8, Residency::ANY, 16), b_avx2, false);
        CHECK(!sat(rep) && why(rep) == UnsatReason::FIXED_REG_WRONG_CLASS,
              "pin XMM0 con clase GP no da WRONG_CLASS");
    }
    // XMM14 (id 30) es scratch -> UNUSABLE.
    {
        auto rep = requirements_satisfiable(
            req(ResourceClass::FP_VECTOR, ViewWidth::W8, Residency::ANY, 30), b_avx2, false);
        CHECK(!sat(rep) && why(rep) == UnsatReason::FIXED_REG_UNUSABLE,
              "pin a scratch XMM14 no da UNUSABLE");
    }
    // RSP (id 4) es frame (ABI) -> UNUSABLE.
    {
        auto rep = requirements_satisfiable(
            req(ResourceClass::GP, ViewWidth::W8, Residency::ANY, 4), b_avx2, false);
        CHECK(!sat(rep) && why(rep) == UnsatReason::FIXED_REG_UNUSABLE,
              "pin a RSP (frame) no da UNUSABLE");
    }
    // id 60 inexistente en x86-64 -> MISSING.
    {
        auto rep = requirements_satisfiable(
            req(ResourceClass::GP, ViewWidth::W8, Residency::ANY, 60), b_avx2, false);
        CHECK(!sat(rep) && why(rep) == UnsatReason::FIXED_REG_MISSING,
              "pin a id inexistente no da MISSING");
    }
    // XMM10 (id 26) es VEC_ACC (OPTIMIZATION): SI puede alojar un valor pinado.
    CHECK(sat(requirements_satisfiable(
              req(ResourceClass::FP_VECTOR, ViewWidth::W8, Residency::ANY, 26), b_avx2, false)),
          "pin a VEC_ACC XMM10 no OK (deberia poder alojar valor)");
    // XMM0 con W64 en avx2 -> el fixed_reg no soporta el ancho.
    {
        auto rep = requirements_satisfiable(
            req(ResourceClass::FP_VECTOR, ViewWidth::W64, Residency::ANY, 16), b_avx2, false);
        CHECK(!sat(rep) && why(rep) == UnsatReason::FIXED_REG_WIDTH_UNSUPPORTED,
              "pin XMM0 W64 en avx2 no da WIDTH_UNSUPPORTED");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
