/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/analysis/test_profile_facts.cpp
 * @brief Test de compute_profile_facts (profiler centralizado, fact Tipo B):
 *        trip-count por bucle (v1 aproximado) + peso de ejecucion por bloque.
 */

#include "analysis/derived/profile_facts.h"
#include "analysis/facts/loop_facts.h"
#include "ir/ssa_ir.h"

#include <cstdio>

using namespace analysis;
using ir::IrBlock;
using ir::IrBlockId;
using ir::IrInstr;
using ir::IrOp;
using ir::IrType;

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

static IrInstr br(IrBlockId t) {
    IrInstr i;
    i.op = IrOp::BR;
    i.type = IrType::VOID;
    i.dst = ir::IR_NO_VALUE;
    i.target_block = t;
    return i;
}
static IrInstr brcond(IrBlockId tt, IrBlockId ff, uint32_t line) {
    IrInstr i;
    i.op = IrOp::BR_COND;
    i.type = IrType::VOID;
    i.dst = ir::IR_NO_VALUE;
    i.operands = {0};
    i.target_block = tt;
    i.false_block = ff;
    i.source_line = line;
    return i;
}
static IrInstr ret() {
    IrInstr i;
    i.op = IrOp::RET;
    i.type = IrType::VOID;
    i.dst = ir::IR_NO_VALUE;
    return i;
}
static IrBlock block(IrBlockId id, const char *name, IrInstr term) {
    IrBlock b;
    b.id = id;
    b.name = name;
    b.instrs.push_back(term);
    return b;
}

int main() {
    std::printf("=== test_profile_facts (profiler centralizado, Tipo B) ===\n");

    // CFG anidado (mismo que LoopFacts) con lineas en los branches de cabecera.
    ir::IrFunction fn;
    fn.name = "nested";
    fn.blocks.push_back(block(0, "entry", br(1)));
    fn.blocks.push_back(block(1, "outer_h", brcond(2, 5, /*line=*/100)));
    fn.blocks.push_back(block(2, "inner_h", brcond(3, 4, /*line=*/200)));
    fn.blocks.push_back(block(3, "inner_b", br(2)));
    fn.blocks.push_back(block(4, "outer_latch", br(1)));
    fn.blocks.push_back(block(5, "exit", ret()));
    LoopFacts loops = compute_loop_facts(fn);

    // --- Sin perfil -> has_profile=false, pesos 0 (fallback estatico) ---
    std::printf("\n[sin perfil]\n");
    {
        BranchProfile empty;
        ProfileFacts pf = compute_profile_facts(fn, loops, empty);
        CHECK(!pf.has_profile, "has_profile true sin perfil");
        CHECK(pf.weight_of(3) == 0.0,
              "peso != 0 sin perfil (deberia caer al estatico)");
    }

    // --- Con perfil: outer trip 10 (1000/100), inner trip 5 (5000/1000) ---
    std::printf("\n[con perfil: trip-count + pesos]\n");
    {
        BranchProfile prof;
        prof.set(100, 1000, 100);  // outer_h: max/min = 10
        prof.set(200, 5000, 1000); // inner_h: max/min = 5
        ProfileFacts pf = compute_profile_facts(fn, loops, prof);
        CHECK(pf.has_profile, "has_profile false con perfil");

        const uint32_t outer_id = loops.innermost(1); // bloque 1 solo en outer
        const uint32_t inner_id = loops.innermost(3); // bloque 3 en inner
        CHECK(pf.trip_of(outer_id) == 10.0, "trip outer != 10");
        CHECK(pf.trip_of(inner_id) == 5.0, "trip inner != 5");

        // Pesos: entry/exit fuera de bucle = 1; outer_h = 10; inner_h/inner_b =
        // 10*5 = 50; outer_latch = 10.
        CHECK(pf.weight_of(0) == 1.0, "entry no peso 1");
        CHECK(pf.weight_of(1) == 10.0, "outer_h no peso 10");
        CHECK(pf.weight_of(2) == 50.0, "inner_h no peso 50 (10*5)");
        CHECK(pf.weight_of(3) == 50.0, "inner_b no peso 50");
        CHECK(pf.weight_of(4) == 10.0, "outer_latch no peso 10");
        CHECK(pf.weight_of(5) == 1.0, "exit no peso 1");
    }

    // --- Rama de salida nunca vista -> trip saturado, no cero ---
    std::printf("\n[rama de salida no vista -> trip alto]\n");
    {
        BranchProfile prof;
        prof.set(100, 1000000, 0); // nunca salio -> trip = kMaxTrip
        prof.set(200, 3, 1);
        ProfileFacts pf = compute_profile_facts(fn, loops, prof);
        const uint32_t outer_id = loops.innermost(1);
        CHECK(pf.trip_of(outer_id) > 1000.0,
              "trip con salida-0 no saturo alto");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
