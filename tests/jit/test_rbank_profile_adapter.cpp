/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_profile_adapter.cpp
 * @brief Test del ProfileAdapter (Fase 0.25): ProfileFacts -> execution_weight,
 *        y que el OptimizationContext PREFIERE el peso medido sobre el
 * estatico.
 */

#include "analysis/derived/profile_facts.h"
#include "analysis/facts/loop_facts.h"
#include "ir/ssa_ir.h"
#include "codegen/rbank/adapters/profile_adapter.h"
#include "codegen/rbank/optimization_context.h"
#include "codegen/rbank/value_requirements.h"

#include <cstdio>

using namespace jit;
using namespace codegen::rbank;
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
    std::printf("=== test_rbank_profile_adapter (Fase 0.25) ===\n");

    ir::IrFunction fn;
    fn.name = "loop1";
    fn.blocks.push_back(block(0, "entry", br(1)));
    fn.blocks.push_back(block(1, "header", brcond(2, 3, /*line=*/50)));
    fn.blocks.push_back(block(2, "body", br(1)));
    fn.blocks.push_back(block(3, "exit", ret()));
    analysis::LoopFacts loops = analysis::compute_loop_facts(fn);
    analysis::BranchProfile prof;
    prof.set(50, 700, 100); // trip = 7

    analysis::ProfileFacts pf =
        analysis::compute_profile_facts(fn, loops, prof);

    std::printf("\n[ProfileAdapter -> execution_weight]\n");
    {
        // Valor definido en el body (bloque 2), peso = trip = 7.
        ValueRequirements r;
        r.loop_depth = 1;
        populate_profile_requirements(r, pf, /*def_block=*/2);
        CHECK(r.execution_weight == 7.0, "execution_weight del body != 7");
    }

    std::printf("\n[el contexto PREFIERE el peso medido]\n");
    {
        PhysicalRegisterBank bank = physical_bank_x86_64(true, [] {
            BackendCaps c{};
            c.sse2 = true;
            return c;
        }());
        ConstraintSet cs;
        OptimizationContext ctx = make_context(bank, cs);

        ValueRequirements measured;
        measured.loop_depth = 1;
        populate_profile_requirements(measured, pf, 2); // execution_weight = 7
        // Con perfil -> usa 7 (no el estatico 10^1 = 10).
        CHECK(ctx.execution_weight(measured) == 7.0,
              "el contexto no prefirio el peso medido");

        ValueRequirements no_prof;
        no_prof.loop_depth = 2; // sin execution_weight
        // Sin perfil -> estatico 10^2 = 100.
        CHECK(ctx.execution_weight(no_prof) == 100.0,
              "sin perfil no cae al estatico 10^depth");
    }

    std::printf("\n[solo toca execution_weight]\n");
    {
        ValueRequirements r;
        r.value_id = 3;
        r.crosses_call = true;
        r.loop_depth = 1;
        r.cls = ResourceClass::FP_VECTOR;
        populate_profile_requirements(r, pf, 2);
        CHECK(r.execution_weight == 7.0, "no actualizo execution_weight");
        CHECK(r.value_id == 3 && r.crosses_call && r.loop_depth == 1 &&
                  r.cls == ResourceClass::FP_VECTOR,
              "el ProfileAdapter toco campos ajenos");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
