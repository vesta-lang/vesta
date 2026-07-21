/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_liveness_adapter.cpp
 * @brief Test del LivenessAdapter (Fase 0.25): liveness + calls -> crosses_call.
 *        Dos niveles: (a) traduccion pura (intervalo + posiciones sinteticas);
 *        (b) INTEGRACION con un IrFunction real + compute_liveness, que valida
 *        que las posiciones de call son consistentes con def/end (linealizacion).
 */

#include "ir/liveness.h"
#include "ir/ssa_ir.h"
#include "codegen/rbank/adapters/liveness_adapter.h"
#include "codegen/rbank/value_requirements.h"

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

static ir::IrInstr mk(ir::IrOp op, ir::IrType t, ir::IrValueId dst,
                      std::vector<ir::IrValueId> ops = {}, uint64_t imm = 0,
                      std::string fn = "") {
    ir::IrInstr i;
    i.op = op; i.type = t; i.dst = dst;
    i.operands = std::move(ops); i.imm = imm; i.func_name = std::move(fn);
    return i;
}

int main() {
    std::printf("=== test_rbank_liveness_adapter (Fase 0.25.2) ===\n");

    // --- ir_op_is_call ---
    std::printf("\n[ir_op_is_call]\n");
    CHECK(ir_op_is_call(ir::IrOp::CALL) && ir_op_is_call(ir::IrOp::CALLN) &&
          ir_op_is_call(ir::IrOp::CALLVIRT) && ir_op_is_call(ir::IrOp::CALLCLOSURE),
          "op de llamada no reconocida");
    CHECK(!ir_op_is_call(ir::IrOp::ADD) && !ir_op_is_call(ir::IrOp::CONST) &&
          !ir_op_is_call(ir::IrOp::RET),
          "op no-llamada marcada como llamada");

    // --- interval_covers (canonico: def <= p <= end) ---
    std::printf("\n[interval_covers inclusivo]\n");
    {
        ir::LiveInterval iv{0, 2, 8};
        CHECK(interval_covers(iv, 2) && interval_covers(iv, 5) && interval_covers(iv, 8),
              "covers de borde/interior falla");
        CHECK(!interval_covers(iv, 1) && !interval_covers(iv, 9),
              "covers fuera de rango");
    }

    // --- populate: traduccion pura ---
    std::printf("\n[crosses_call: traduccion pura]\n");
    {
        ir::LiveInterval iv{0, 2, 8};
        ValueRequirements r; r.loop_depth = 9; r.is_gc = true; // otros campos
        populate_liveness_requirements(r, iv, {5});
        CHECK(r.crosses_call, "call en [def,end] no marca crosses_call");
        CHECK(r.loop_depth == 9 && r.is_gc, "toco campos ajenos");
        ValueRequirements r2;
        populate_liveness_requirements(r2, iv, {10});
        CHECK(!r2.crosses_call, "call fuera del intervalo marca crosses_call");
        ValueRequirements r3;
        populate_liveness_requirements(r3, iv, {2}); // ==def (inclusivo)
        CHECK(r3.crosses_call, "call en def no cuenta (deberia, es inclusivo)");
    }

    // --- INTEGRACION: IrFunction real + compute_liveness ---
    std::printf("\n[integracion: IrFunction real]\n");
    {
        using ir::IrOp; using ir::IrType;
        ir::IrFunction fn;
        fn.name = "test"; fn.ret_type = IrType::I64;
        fn.values.resize(3);
        for (uint32_t i = 0; i < 3; ++i) { fn.values[i].id = i; fn.values[i].type = IrType::I64; }
        ir::IrBlock blk; blk.id = 0; blk.name = "entry";
        blk.instrs.push_back(mk(IrOp::CONST, IrType::I64, 0, {}, 5));       // pos 0: def v0
        blk.instrs.push_back(mk(IrOp::CALL, IrType::VOID, ir::IR_NO_VALUE, {}, 0, "foo")); // pos 1: call
        blk.instrs.push_back(mk(IrOp::CONST, IrType::I64, 1, {}, 7));       // pos 2: def v1 (tras call)
        blk.instrs.push_back(mk(IrOp::ADD, IrType::I64, 2, {0, 1}));        // pos 3: usa v0 y v1
        blk.instrs.push_back(mk(IrOp::RET, IrType::I64, ir::IR_NO_VALUE, {2})); // pos 4: usa v2
        fn.blocks.push_back(std::move(blk));

        ir::LivenessResult live = ir::compute_liveness(fn);
        std::vector<uint32_t> calls = collect_call_positions(fn, live);
        CHECK(calls.size() == 1 && calls[0] == 1,
              "posicion de call no consistente con la linealizacion");

        // v0 [0,3] cubre la call (1) -> cruza; v1 [2,3] y v2 [3,4] no.
        bool crosses[3] = {false, false, false};
        bool seen[3] = {false, false, false};
        for (const ir::LiveInterval &iv : live.intervals) {
            if (iv.id > 2) continue;
            ValueRequirements r; r.value_id = iv.id;
            populate_liveness_requirements(r, iv, calls);
            crosses[iv.id] = r.crosses_call;
            seen[iv.id] = true;
        }
        CHECK(seen[0] && seen[1] && seen[2], "faltan intervalos de v0/v1/v2");
        CHECK(crosses[0], "v0 (vivo a traves del call) no cruza");
        CHECK(!crosses[1], "v1 (definido tras el call) cruza indebidamente");
        CHECK(!crosses[2], "v2 (tras el call) cruza indebidamente");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
