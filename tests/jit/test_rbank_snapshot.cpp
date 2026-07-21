/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_snapshot.cpp
 * @brief Test de FunctionSnapshot: build_snapshot (fotografia completa de una
 *        funcion) + validate (autocertificacion de cada Fact + auditor).  El
 *        compilador auditandose a si mismo.
 */

#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/snapshot_builder.h"
#include "ir/ssa_ir.h"

#include <cstdio>

using namespace jit;
using namespace jit::rbank;
using ir::IrBlock;
using ir::IrBlockId;
using ir::IrInstr;
using ir::IrOp;
using ir::IrType;
using ir::IrValue;
using ir::IrValueId;

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

static IrInstr mk(IrOp op, IrType t, IrValueId dst,
                  std::vector<IrValueId> ops = {}, uint64_t imm = 0,
                  std::string fn = "") {
    IrInstr i; i.op = op; i.type = t; i.dst = dst;
    i.operands = std::move(ops); i.imm = imm; i.func_name = std::move(fn);
    return i;
}
static IrValue mkval(IrValueId id, IrType t, bool is_const = false,
                     bool is_param = false) {
    IrValue v; v.id = id; v.type = t; v.is_const = is_const; v.is_param = is_param;
    return v;
}

/** @brief Funcion con un bucle y un acumulador f64 que cruza el back-edge. */
static ir::IrFunction make_loop_fn() {
    ir::IrFunction fn; fn.name = "loopfp"; fn.ret_type = IrType::F64;
    fn.values = {mkval(0, IrType::I64, false, true), mkval(1, IrType::F64, true),
                 mkval(2, IrType::F64)};
    fn.params = {0};
    IrBlock b0; b0.id = 0; b0.name = "entry";
    b0.instrs.push_back(mk(IrOp::CONST, IrType::F64, 1, {}, 0x3FF8000000000000ull));
    b0.instrs.push_back(mk(IrOp::BR, IrType::VOID, ir::IR_NO_VALUE));
    b0.instrs.back().target_block = 1;
    IrBlock b1; b1.id = 1; b1.name = "header";
    b1.instrs.push_back(mk(IrOp::ADD, IrType::F64, 2, {1, 1}));
    b1.instrs.push_back(mk(IrOp::BR_COND, IrType::VOID, ir::IR_NO_VALUE, {0}));
    b1.instrs.back().target_block = 2; b1.instrs.back().false_block = 3;
    IrBlock b2; b2.id = 2; b2.name = "body";
    b2.instrs.push_back(mk(IrOp::BR, IrType::VOID, ir::IR_NO_VALUE));
    b2.instrs.back().target_block = 1;
    IrBlock b3; b3.id = 3; b3.name = "exit";
    b3.instrs.push_back(mk(IrOp::RET, IrType::F64, ir::IR_NO_VALUE, {2}));
    fn.blocks = {std::move(b0), std::move(b1), std::move(b2), std::move(b3)};
    return fn;
}

int main() {
    std::printf("=== test_rbank_snapshot (fotografia + autocertificacion) ===\n");

    ir::IrFunction fn = make_loop_fn();
    FunctionSnapshot snap = build_snapshot(fn);

    std::printf("\n[la fotografia contiene los Facts + valores]\n");
    CHECK(snap.fn == &fn, "snapshot no apunta a la funcion");
    CHECK(snap.loops.loop_count == 1, "no detecto el bucle");
    CHECK(!snap.live.intervals.empty(), "sin intervalos de liveness");
    CHECK(!snap.values.empty(), "sin ValueRequirements");
    // El acumulador v2 (f64, en el loop) esta en la foto con sus hechos.
    const ValueRequirements *v2 = nullptr;
    for (const ValueRequirements &r : snap.values) if (r.value_id == 2) v2 = &r;
    CHECK(v2 && v2->cls == ResourceClass::FP_VECTOR && v2->loop_depth == 1,
          "v2 (f64 en loop) mal en la foto");

    PhysicalRegisterBank bank = physical_bank_x86_64(
        true, [] { BackendCaps c{}; c.sse2 = true; return c; }());

    std::printf("\n[autocertificacion: conocimiento SANO]\n");
    {
        FunctionSnapshot::ValidationReport rep = snap.validate(bank);
        CHECK(rep.ok(), "la foto valida reporto incoherencias");
        std::printf("  fact_issues=%zu value_issues=%zu (todo sano)\n",
                    rep.fact_issues.size(), rep.value_issues.size());
    }

    std::printf("\n[el auditor CAZA un Fact corrupto]\n");
    {
        FunctionSnapshot bad = build_snapshot(fn);
        // Corrompemos LoopFacts: header (block1) in_loop pero depth 0.
        bad.loops.loop_depth[1] = 0; // mientras in_loop[1] sigue 1 -> mismatch
        FunctionSnapshot::ValidationReport rep = bad.validate(bank);
        bool caught = false;
        for (const analysis::FactIssue &i : rep.fact_issues)
            if (i.check == analysis::FactCheck::LOOP_DEPTH_INLOOP_MISMATCH) caught = true;
        CHECK(!rep.ok() && caught, "no cazo el LoopFacts corrupto");
    }

    std::printf("\n[el auditor CAZA un valor imposible]\n");
    {
        FunctionSnapshot bad = build_snapshot(fn);
        // Pin de un valor FP a un registro GP (RAX).
        for (ValueRequirements &r : bad.values)
            if (r.cls == ResourceClass::FP_VECTOR) { r.fixed_reg = 0; break; }
        FunctionSnapshot::ValidationReport rep = bad.validate(bank);
        bool caught = false;
        for (const RequirementIssue &i : rep.value_issues)
            if (i.reason == UnsatReason::FIXED_REG_WRONG_CLASS) caught = true;
        CHECK(!rep.ok() && caught, "no cazo el valor imposible");
    }

    std::printf("\n[SnapshotBuilder: subconjunto de Facts]\n");
    {
        // Solo Loops -> ni liveness ni values.
        FunctionSnapshot only_loops = SnapshotBuilder().enable(Fact::Loops).build(fn);
        CHECK(only_loops.loops.loop_count == 1, "builder solo-Loops sin bucles");
        CHECK(only_loops.live.intervals.empty(), "builder solo-Loops computo liveness");
        CHECK(only_loops.values.empty(), "builder solo-Loops computo values");
    }

    std::printf("\n[SnapshotBuilder: resuelve dependencias]\n");
    {
        // Pedir Values debe activar Liveness + Loops por dependencia.
        FunctionSnapshot s = SnapshotBuilder().enable(Fact::Values).build(fn);
        CHECK(!s.values.empty(), "builder Values sin valores");
        CHECK(!s.live.intervals.empty(),
              "builder Values no resolvio la dependencia Liveness");
        CHECK(s.loops.loop_count == 1,
              "builder Values no resolvio la dependencia Loops");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
