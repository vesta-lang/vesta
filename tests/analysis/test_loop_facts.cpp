/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/analysis/test_loop_facts.cpp
 * @brief Test de compute_loop_facts: deteccion unificada de bucles (profundidad
 *        + header + in_loop + id) sobre CFGs sinteticos (sin loop, un loop,
 *        loops anidados).
 */

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
static IrInstr brcond(IrBlockId tt, IrBlockId ff) {
    IrInstr i;
    i.op = IrOp::BR_COND;
    i.type = IrType::VOID;
    i.dst = ir::IR_NO_VALUE;
    i.operands = {0};
    i.target_block = tt;
    i.false_block = ff;
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
    std::printf("=== test_loop_facts (Fase 0.25: LoopFacts) ===\n");

    // --- CFG sin bucles: entry -> exit ---
    std::printf("\n[sin bucles]\n");
    {
        ir::IrFunction fn;
        fn.name = "noloop";
        fn.blocks.push_back(block(0, "entry", br(1)));
        fn.blocks.push_back(block(1, "exit", ret()));
        LoopFacts f = compute_loop_facts(fn);
        CHECK(f.loop_count == 0, "loop_count != 0");
        CHECK(!f.inside(0) && !f.inside(1),
              "bloques marcados in_loop sin bucle");
        CHECK(f.depth_of(0) == 0 && f.depth_of(1) == 0, "profundidad != 0");
    }

    // --- Un bucle: entry -> header <-> body, header -> exit ---
    std::printf("\n[un bucle]\n");
    {
        ir::IrFunction fn;
        fn.name = "oneloop";
        fn.blocks.push_back(block(0, "entry", br(1)));
        fn.blocks.push_back(block(1, "header", brcond(2, 3)));
        fn.blocks.push_back(block(2, "body", br(1))); // back-edge 2->1
        fn.blocks.push_back(block(3, "exit", ret()));
        LoopFacts f = compute_loop_facts(fn);
        CHECK(f.loop_count == 1, "loop_count != 1");
        CHECK(f.header_of(1), "header no detectado");
        CHECK(f.depth_of(1) == 1 && f.depth_of(2) == 1, "cuerpo no depth 1");
        CHECK(f.inside(1) && f.inside(2), "header/body no in_loop");
        CHECK(f.depth_of(0) == 0 && f.depth_of(3) == 0,
              "entry/exit no depth 0");
        CHECK(!f.header_of(2), "body marcado header");
    }

    // --- Bucles anidados ---
    std::printf("\n[bucles anidados]\n");
    {
        // 0 entry -> 1 outer_h
        // 1 outer_h -> 2 inner_h | 5 exit
        // 2 inner_h -> 3 inner_b | 4 outer_latch
        // 3 inner_b -> 2 (back inner)
        // 4 outer_latch -> 1 (back outer)
        // 5 exit ret
        ir::IrFunction fn;
        fn.name = "nested";
        fn.blocks.push_back(block(0, "entry", br(1)));
        fn.blocks.push_back(block(1, "outer_h", brcond(2, 5)));
        fn.blocks.push_back(block(2, "inner_h", brcond(3, 4)));
        fn.blocks.push_back(block(3, "inner_b", br(2)));
        fn.blocks.push_back(block(4, "outer_latch", br(1)));
        fn.blocks.push_back(block(5, "exit", ret()));
        LoopFacts f = compute_loop_facts(fn);
        CHECK(f.loop_count == 2, "loop_count != 2");
        CHECK(f.depth_of(0) == 0, "entry no depth 0");
        CHECK(f.depth_of(1) == 1, "outer_h no depth 1");
        CHECK(f.depth_of(2) == 2, "inner_h no depth 2");
        CHECK(f.depth_of(3) == 2, "inner_b no depth 2");
        CHECK(f.depth_of(4) == 1, "outer_latch no depth 1");
        CHECK(f.depth_of(5) == 0, "exit no depth 0");
        CHECK(f.header_of(1) && f.header_of(2), "headers no detectados");
        // loop_id: el bloque mas interno apunta al bucle mas pequeno.
        CHECK(f.innermost(3) == f.innermost(2),
              "inner_b/inner_h distinto bucle interno");
        CHECK(f.innermost(3) != f.innermost(4),
              "inner y outer comparten id interno");
        // parent_loop: el bucle interno tiene como padre al externo; el externo
        // no.
        const uint32_t inner_id = f.innermost(3);
        const uint32_t outer_id = f.innermost(1);
        CHECK(f.parent_of(inner_id) == outer_id,
              "padre del bucle interno no es el externo");
        CHECK(f.parent_of(outer_id) == LoopFacts::NO_LOOP,
              "el bucle externo tiene padre");
        CHECK(f.header_block_of(inner_id) == 2 &&
                  f.header_block_of(outer_id) == 1,
              "header_block_of incorrecto");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
