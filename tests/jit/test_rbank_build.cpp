/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_build.cpp
 * @brief Test de build_value_requirements (snapshot vivo) + audit_requirements
 *        (el compilador auditandose): corre los adaptadores sobre una funcion
 *        real y comprueba los hechos por valor + que ningun valor es imposible.
 */

#include "ir/ssa_ir.h"
#include "codegen/rbank/build_requirements.h"
#include "codegen/rbank/physical_bank.h"

#include <cstdio>

using namespace jit;
using namespace codegen::rbank;
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
                     bool is_gc = false, bool is_param = false) {
    IrValue v; v.id = id; v.type = t; v.is_const = is_const;
    v.is_gc_object = is_gc; v.is_param = is_param; return v;
}

/** @brief Busca el ValueRequirements de @p id en el snapshot. */
static const ValueRequirements *find(const std::vector<ValueRequirements> &rs,
                                     uint32_t id) {
    for (const ValueRequirements &r : rs)
        if (r.value_id == id) return &r;
    return nullptr;
}

int main() {
    std::printf("=== test_rbank_build (snapshot vivo + auditor) ===\n");

    // Funcion:  v0=param i64;  v1=const f64;  v3=const handle;  loop:
    //   v2 = v1 + v1 (f64, en el loop);  call foo(v2);  ret v2 (cruza el call).
    ir::IrFunction fn; fn.name = "sample"; fn.ret_type = IrType::F64;
    fn.values = {mkval(0, IrType::I64, false, false, true),
                 mkval(1, IrType::F64, true),
                 mkval(2, IrType::F64),
                 mkval(3, IrType::HANDLE, true)};
    fn.params = {0};

    IrBlock b0; b0.id = 0; b0.name = "entry";
    b0.instrs.push_back(mk(IrOp::CONST, IrType::F64, 1, {}, 0x3FF8000000000000ull));
    b0.instrs.push_back(mk(IrOp::CONST, IrType::HANDLE, 3, {}, 7));
    b0.instrs.push_back(mk(IrOp::BR, IrType::VOID, ir::IR_NO_VALUE));
    b0.instrs.back().target_block = 1;

    IrBlock b1; b1.id = 1; b1.name = "header";
    b1.instrs.push_back(mk(IrOp::ADD, IrType::F64, 2, {1, 1}));
    b1.instrs.push_back(mk(IrOp::BR_COND, IrType::VOID, ir::IR_NO_VALUE, {0}));
    b1.instrs.back().target_block = 2; b1.instrs.back().false_block = 3;

    IrBlock b2; b2.id = 2; b2.name = "body";
    b2.instrs.push_back(mk(IrOp::CALL, IrType::VOID, ir::IR_NO_VALUE, {2}, 0, "foo"));
    b2.instrs.push_back(mk(IrOp::BR, IrType::VOID, ir::IR_NO_VALUE));
    b2.instrs.back().target_block = 1;

    IrBlock b3; b3.id = 3; b3.name = "exit";
    b3.instrs.push_back(mk(IrOp::RET, IrType::F64, ir::IR_NO_VALUE, {2}));

    fn.blocks = {std::move(b0), std::move(b1), std::move(b2), std::move(b3)};

    std::vector<ValueRequirements> reqs = build_value_requirements(fn);

    std::printf("\n[snapshot: hechos por valor real]\n");
    {
        const ValueRequirements *v1 = find(reqs, 1);
        CHECK(v1 && v1->cls == ResourceClass::FP_VECTOR && v1->width == ViewWidth::W8,
              "v1 (f64) no es FP/W8");
        CHECK(v1 && v1->rematerializable, "v1 (const) no es rematerializable");
        CHECK(v1 && v1->loop_depth == 0, "v1 (block0) no depth 0");

        const ValueRequirements *v2 = find(reqs, 2);
        CHECK(v2 && v2->cls == ResourceClass::FP_VECTOR && v2->width == ViewWidth::W8,
              "v2 (f64) no es FP/W8");
        CHECK(v2 && v2->loop_depth == 1, "v2 (en el loop) no depth 1");
        CHECK(v2 && v2->crosses_call, "v2 (vivo a traves del call) no cruza");

        const ValueRequirements *v3 = find(reqs, 3);
        CHECK(v3 && v3->cls == ResourceClass::GP && v3->width == ViewWidth::W4,
              "v3 (handle) no es GP/W4");
        CHECK(v3 && v3->is_gc, "v3 (handle) no marcado is_gc");

        const ValueRequirements *v0 = find(reqs, 0);
        CHECK(v0 && v0->cls == ResourceClass::GP && v0->width == ViewWidth::W8,
              "v0 (param i64) no es GP/W8");
    }

    std::printf("\n[auditor: ningun valor imposible en x86-64]\n");
    {
        PhysicalRegisterBank bank = physical_bank_x86_64(
            true, [] { BackendCaps c{}; c.sse2 = true; return c; }());
        std::vector<RequirementIssue> issues = audit_requirements(reqs, bank);
        CHECK(issues.empty(), "el auditor reporto valores imposibles (no deberia)");
        std::printf("  auditados %zu valores, %zu incoherencias\n",
                    reqs.size(), issues.size());
    }

    std::printf("\n[auditor CAZA una incoherencia inyectada]\n");
    {
        // Inyectamos un fixed_reg incompatible: un valor FP pinado a un GP (RAX).
        std::vector<ValueRequirements> bad = reqs;
        for (ValueRequirements &r : bad)
            if (r.cls == ResourceClass::FP_VECTOR) { r.fixed_reg = 0; break; } // RAX
        PhysicalRegisterBank bank = physical_bank_x86_64(
            true, [] { BackendCaps c{}; c.sse2 = true; return c; }());
        std::vector<RequirementIssue> issues = audit_requirements(bad, bank);
        CHECK(!issues.empty() && issues[0].reason == UnsatReason::FIXED_REG_WRONG_CLASS,
              "el auditor no cazo el pin FP->GP incompatible");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
