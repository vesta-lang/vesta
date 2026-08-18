/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_vec_acc_demand.cpp
 * @brief Test de REGRESION de la reserva VEC_ACC DEMAND-DRIVEN (Fase 2).
 *
 * Blinda la propiedad del allocator: XMM10-13 (ids 26-29) solo se reservan para
 * los acumuladores vectoriales VEC_ACC en funciones que USAN el path de
 * reduccion; las funciones escalares FP los tienen ASIGNABLES (14 lanes en vez
 * de 10).  Si alguien vuelve a reservar VEC_ACC de forma PERMANENTE (revierte
 * el fix), el pool "libre" dejaria de tener 14 lanes y este test fallaria -> CI
 * lo detecta.
 *
 * Se testea la PROPIEDAD (conteo de lanes del banco), no el numero de spills de
 * un disasm concreto (mas fragil).  El resultado del programa NO cambia con el
 * fix (verificado aparte en e2e/diff_harness); lo que cambia es cuantos valores
 * caben en registro.
 */

#include "jit/target_reginfo.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace jit;

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

static bool has(const std::vector<uint8_t> &v, uint8_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

int main() {
    std::printf(
        "=== test_vec_acc_demand (reserva VEC_ACC demand-driven) ===\n");
    const size_t FP = static_cast<size_t>(RegClass::FP);

    // Ids: XMM0=16 .. XMM9=25, XMM10=26 .. XMM13=29, XMM14=30, XMM15=31.
    for (bool sysv : {true, false}) {
        const char *abi = sysv ? "sysv" : "win64";
        const TargetRegInfo &res =
            target_x86_64_abi(sysv, /*reserve_vec_acc=*/true);
        const TargetRegInfo &fre =
            target_x86_64_abi(sysv, /*reserve_vec_acc=*/false);

        std::printf("\n[%s] reservado=%zu lanes  libre=%zu lanes\n", abi,
                    res.allocatable[FP].size(), fre.allocatable[FP].size());

        // RESERVADO (funciones con reduccion vectorial): XMM0..9 = 10 lanes.
        CHECK(res.allocatable[FP].size() == 10,
              "pool reservado != 10 lanes FP");
        for (uint8_t id = 26; id <= 29; ++id)
            CHECK(!has(res.allocatable[FP], id),
                  "XMM10-13 asignables en el pool RESERVADO (deben quedar para "
                  "VEC_ACC)");

        // LIBRE (funciones escalares FP): XMM0..13 = 14 lanes (el fix).
        CHECK(
            fre.allocatable[FP].size() == 14,
            "pool libre != 14 lanes FP -- el fix demand-driven fue revertido?");
        for (uint8_t id = 26; id <= 29; ++id)
            CHECK(
                has(fre.allocatable[FP], id),
                "XMM10-13 NO asignables en el pool LIBRE (regresion del fix)");

        // XMM14/15 (scratch del rewrite) NUNCA asignables, en ambos pools.
        for (uint8_t id = 30; id <= 31; ++id)
            CHECK(!has(res.allocatable[FP], id) &&
                      !has(fre.allocatable[FP], id),
                  "XMM14/15 asignables (deben ser scratch en ambos pools)");

        // XMM0..9 asignables SIEMPRE (nucleo comun).
        for (uint8_t id = 16; id <= 25; ++id)
            CHECK(has(res.allocatable[FP], id) && has(fre.allocatable[FP], id),
                  "XMM0-9 no asignables (nucleo comun roto)");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
