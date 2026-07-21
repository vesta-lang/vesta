/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_loop_adapter.cpp
 * @brief Test del LoopAdapter (Fase 0.25.4): LoopFacts -> loop_depth.  Integra
 *        compute_loop_facts real con el adaptador (ridiculamente simple).
 */

#include "analysis/facts/loop_facts.h"
#include "ir/ssa_ir.h"
#include "jit/rbank/adapters/loop_adapter.h"
#include "jit/rbank/value_requirements.h"

#include <cstdio>

using namespace jit;
using namespace jit::rbank;
using ir::IrBlock;
using ir::IrBlockId;
using ir::IrInstr;
using ir::IrOp;
using ir::IrType;

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

static IrInstr br(IrBlockId t) {
    IrInstr i; i.op = IrOp::BR; i.type = IrType::VOID; i.dst = ir::IR_NO_VALUE;
    i.target_block = t; return i;
}
static IrInstr brcond(IrBlockId tt, IrBlockId ff) {
    IrInstr i; i.op = IrOp::BR_COND; i.type = IrType::VOID; i.dst = ir::IR_NO_VALUE;
    i.operands = {0}; i.target_block = tt; i.false_block = ff; return i;
}
static IrInstr ret() {
    IrInstr i; i.op = IrOp::RET; i.type = IrType::VOID; i.dst = ir::IR_NO_VALUE;
    return i;
}
static IrBlock block(IrBlockId id, const char *name, IrInstr term) {
    IrBlock b; b.id = id; b.name = name; b.instrs.push_back(term); return b;
}

int main() {
    std::printf("=== test_rbank_loop_adapter (Fase 0.25.4) ===\n");

    // CFG anidado: 0 entry / 1 outer_h / 2 inner_h / 3 inner_b / 4 latch / 5 exit.
    ir::IrFunction fn; fn.name = "nested";
    fn.blocks.push_back(block(0, "entry", br(1)));
    fn.blocks.push_back(block(1, "outer_h", brcond(2, 5)));
    fn.blocks.push_back(block(2, "inner_h", brcond(3, 4)));
    fn.blocks.push_back(block(3, "inner_b", br(2)));
    fn.blocks.push_back(block(4, "outer_latch", br(1)));
    fn.blocks.push_back(block(5, "exit", ret()));
    analysis::LoopFacts f = analysis::compute_loop_facts(fn);

    std::printf("\n[LoopAdapter: loop_depth del bloque de definicion]\n");
    {
        // Un valor definido en inner_b (bloque 3, depth 2).
        ValueRequirements r;
        populate_loop_requirements(r, f, /*def_block=*/3);
        CHECK(r.loop_depth == 2, "valor en inner_b no tiene loop_depth 2");
    }
    {
        // Un valor definido en outer_h (bloque 1, depth 1).
        ValueRequirements r;
        populate_loop_requirements(r, f, /*def_block=*/1);
        CHECK(r.loop_depth == 1, "valor en outer_h no tiene loop_depth 1");
    }
    {
        // Un valor definido en entry (bloque 0, fuera de bucle).
        ValueRequirements r;
        populate_loop_requirements(r, f, /*def_block=*/0);
        CHECK(r.loop_depth == 0, "valor en entry no tiene loop_depth 0");
    }

    std::printf("\n[solo toca loop_depth]\n");
    {
        ValueRequirements r;
        r.value_id = 7; r.crosses_call = true; r.rematerializable = true;
        r.cls = ResourceClass::FP_VECTOR;
        populate_loop_requirements(r, f, 2);
        CHECK(r.loop_depth == 2, "no actualizo loop_depth");
        CHECK(r.value_id == 7 && r.crosses_call && r.rematerializable &&
              r.cls == ResourceClass::FP_VECTOR,
              "el LoopAdapter toco campos ajenos");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
