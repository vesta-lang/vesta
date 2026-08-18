/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_remat_facts.cpp
 * @brief RematFacts (Tipo A, IR-driven): recomputabilidad + receta por valor.
 *        Valida el criterio value-only (CONST/aritmetica pura SI; LOAD/CALL/DIV
 * NO)
 *        + la FORMA DE RECETA (op/imm/operands) + el registro en el query
 * system (query<RematFacts>() == compute_remat_facts).
 */

#include "analysis/facts/remat_facts.h"
#include "codegen/rbank/function_snapshot.h"
#include "ir/ssa_ir.h"

#include <cstdio>

using namespace ir;
using analysis::RematFacts;

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

static IrInstr mk(IrOp op, IrValueId dst, std::vector<IrValueId> ops = {},
                  uint64_t imm = 0) {
    IrInstr in;
    in.op = op;
    in.type = IrType::I64;
    in.dst = dst;
    in.operands = std::move(ops);
    in.imm = imm;
    return in;
}

int main() {
    std::printf("=== test_remat_facts (recomputabilidad + receta) ===\n");

    // IR sintetico: value ids 0..5.
    //   1 = const 42            (recomputable)
    //   2 = load [10]           (NO: lee memoria mutable)
    //   3 = add 1, 2            (recomputable value-only)
    //   4 = call                (NO: efectos)
    //   5 = div 1, 2            (NO: trap div-by-zero, conservador)
    IrFunction fn;
    fn.values.resize(6);
    fn.blocks.resize(1);
    fn.blocks[0].id = 0;
    fn.blocks[0].instrs = {
        mk(IrOp::CONST, 1, {}, 42), mk(IrOp::LOAD, 2, {10}),
        mk(IrOp::ADD, 3, {1, 2}),   mk(IrOp::CALL, 4, {}),
        mk(IrOp::DIV, 5, {1, 2}),
    };

    std::printf("\n[criterio value-only]\n");
    const RematFacts f = analysis::compute_remat_facts(fn);
    CHECK(f.recipe.size() == 6, "recipe no dimensionado a fn.values.size()");
    CHECK(f.is_rematerializable(1), "CONST deberia ser recomputable");
    CHECK(!f.is_rematerializable(2), "LOAD NO deberia ser recomputable");
    CHECK(f.is_rematerializable(3), "ADD value-only deberia ser recomputable");
    CHECK(!f.is_rematerializable(4), "CALL NO deberia ser recomputable");
    CHECK(!f.is_rematerializable(5), "DIV (trap) NO deberia ser recomputable");
    CHECK(!f.is_rematerializable(0), "valor 0 sin def NO recomputable");

    std::printf("\n[forma de receta: op / imm / operands]\n");
    const analysis::RematRecipe &r1 = f.recipe_of(1);
    CHECK(r1.valid && r1.op == IrOp::CONST && r1.imm == 42 &&
              r1.operands.empty(),
          "receta del CONST mal (op/imm/operands)");
    const analysis::RematRecipe &r3 = f.recipe_of(3);
    CHECK(r3.valid && r3.op == IrOp::ADD && r3.operands.size() == 2 &&
              r3.operands[0] == 1 && r3.operands[1] == 2,
          "receta del ADD mal (operands que la receta necesita)");
    const analysis::RematRecipe &r2 = f.recipe_of(2);
    CHECK(!r2.valid, "receta del LOAD deberia ser invalida");

    std::printf("\n[query system: query<RematFacts>() == compute]\n");
    {
        codegen::rbank::FunctionSnapshot s;
        s.fn = &fn;
        const RematFacts &q = s.remat_facts(); // azucar de query<RematFacts>()
        CHECK(q.is_rematerializable(1) && q.is_rematerializable(3) &&
                  !q.is_rematerializable(2),
              "query<RematFacts> no coincide con compute_remat_facts");
        CHECK(s.is_computed(codegen::rbank::Fact::Remat),
              "el Fact Remat no quedo materializado tras la query");
        // Segunda consulta = cache hit (no recomputa; mismo objeto).
        CHECK(&s.remat_facts() == &q,
              "la celda LazyFact no cacheo el RematFacts");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
