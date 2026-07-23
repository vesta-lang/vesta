/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_use_def_facts.cpp
 * @brief UseDefFacts (Tipo A, IR-driven): posiciones de uso -> next-use por valor,
 *        en el dominio IR (@c ir::LinearPos).  Valida (1) el next-use lineal +
 *        sentinela de valor muerto, (2) la COHERENCIA CON EL LIVENESS IR en
 *        PHI/back-edge (el arg cuenta en block_end del predecesor, no en la instr
 *        PHI), (3) el func_ptr de CALLIND como uso, y (4) el registro en el query
 *        system (query<UseDefFacts>() == compute).
 */

#include "analysis/facts/use_def_facts.h"
#include "codegen/rbank/function_snapshot.h"
#include "ir/linear_pos.h"
#include "ir/ssa_ir.h"

#include <cstdio>

using namespace ir;
using analysis::UseDefFacts;

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

/// Azucar: envuelve un uint32_t crudo como posicion del dominio IR.  @c ir::LinearPos,
/// NO @c codegen::LinearPos: @c UseDefFacts es un Fact del IR y mezclar dominios es
/// justo el error que el tipo fuerte existe para rechazar en compilacion.
static constexpr ir::LinearPos P(uint32_t v) { return ir::LinearPos{v}; }

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
    std::printf("=== test_use_def_facts (next-use IR, ir::LinearPos) ===\n");

    // -----------------------------------------------------------------------
    // (1) Usos lineales + sentinela de valor muerto.
    //   pos 0: v1 = const 5
    //   pos 1: v2 = const 7
    //   pos 2: v3 = add v1, v2      (usa v1@2, v2@2)
    //   pos 3: v4 = add v3, v1      (usa v3@3, v1@3)
    //   pos 4: ret v4               (usa v4@4)
    // uses: v1={2,3} v2={2} v3={3} v4={4}; v0 sin uso.
    // -----------------------------------------------------------------------
    std::printf("\n[usos lineales + valor muerto]\n");
    {
        IrFunction fn;
        fn.values.resize(5);
        fn.blocks.resize(1);
        fn.blocks[0].id = 0;
        fn.blocks[0].instrs = {
            mk(IrOp::CONST, 1, {}, 5),
            mk(IrOp::CONST, 2, {}, 7),
            mk(IrOp::ADD, 3, {1, 2}),
            mk(IrOp::ADD, 4, {3, 1}),
            mk(IrOp::RET, IR_NO_VALUE, {4}),
        };

        const UseDefFacts f = analysis::compute_use_def(fn);
        CHECK(f.num_instrs == 5, "num_instrs deberia ser 5");
        CHECK(f.num_values() == 5, "num_values deberia ser 5");

        // v1 se usa en 2 y 3.
        CHECK(f.next_use_after(1, P(0)) == P(2), "next_use(v1,0) deberia ser 2");
        CHECK(f.next_use_after(1, P(2)) == P(3), "next_use(v1,2) deberia ser 3");
        CHECK(f.next_use_after(1, P(3)) == UseDefFacts::NO_NEXT_USE,
              "next_use(v1,3) deberia ser NO_NEXT_USE (ya no se usa)");
        CHECK(f.distance_to_next_use(1, P(0)) == 2u, "distancia(v1,0) deberia ser 2");

        // v2 muere en 2: mejor victima que v1 en el punto 2.
        CHECK(f.next_use_after(2, P(2)) == UseDefFacts::NO_NEXT_USE,
              "v2 no deberia tener uso tras su unico uso en 2");
        CHECK(f.distance_to_next_use(2, P(2)) == 0xFFFFFFFFu,
              "v2 muerto en 2 -> distancia infinita (victima ideal)");
        CHECK(f.distance_to_next_use(1, P(2)) == 1u,
              "v1 se reusa en 3 -> distancia 1 (peor victima que v2)");

        // Sin usos: valor 0 (nunca definido ni usado) y consultas fuera de rango.
        CHECK(!f.has_uses(0), "v0 no tiene usos");
        CHECK(f.has_uses(1), "v1 tiene usos");
        CHECK(f.next_use_after(0, P(0)) == UseDefFacts::NO_NEXT_USE,
              "v0 sin usos -> NO_NEXT_USE");
        CHECK(f.next_use_after(IR_NO_VALUE, P(0)) == UseDefFacts::NO_NEXT_USE,
              "IR_NO_VALUE fuera de rango -> NO_NEXT_USE (sin crash)");
    }

    // -----------------------------------------------------------------------
    // (2) PHI / back-edge: coherencia con el liveness IR.  El arg del PHI se
    //     cuenta al FINAL del bloque predecesor, NO en la instruccion PHI.
    //   block0:  pos 0: v1 = const 0
    //   block1:  pos 1: v2 = phi [v1 from b0, v3 from b1]
    //            pos 2: v3 = add v2, v2       (usa v2@2)
    //   block_end: b0=0, b1=2.
    //   -> v1 (arg desde b0) se usa en block_end[b0] = 0  (NO en la PHI @1).
    //   -> v3 (arg desde b1, back-edge) se usa en block_end[b1] = 2.
    // -----------------------------------------------------------------------
    std::printf("\n[PHI/back-edge coherente con el liveness IR]\n");
    {
        IrFunction fn;
        fn.values.resize(4);
        fn.blocks.resize(2);
        fn.blocks[0].id = 0;
        fn.blocks[0].succs = {1};
        fn.blocks[0].instrs = {mk(IrOp::CONST, 1, {}, 0)};

        IrInstr phi = mk(IrOp::PHI, 2);
        phi.phi_args = {IrPhiArg{1, 0}, IrPhiArg{3, 1}};
        fn.blocks[1].id = 1;
        fn.blocks[1].preds = {0, 1};
        fn.blocks[1].succs = {1};
        fn.blocks[1].instrs = {phi, mk(IrOp::ADD, 3, {2, 2})};

        const UseDefFacts f = analysis::compute_use_def(fn);
        CHECK(f.num_instrs == 3, "num_instrs deberia ser 3 (1 + 2)");

        // v1 llega como arg PHI desde block0 -> uso en block_end[0] = 0.
        CHECK(f.off[2] - f.off[1] == 1 && f.use_pos[f.off[1]] == 0,
              "el arg PHI v1 debe contar en block_end[pred]=0, no en la PHI@1");
        // v3 llega como arg PHI (back-edge) desde block1 -> uso en block_end[1] = 2.
        CHECK(f.off[4] - f.off[3] == 1 && f.use_pos[f.off[3]] == 2,
              "el arg PHI v3 (back-edge) debe contar en block_end[b1]=2");
        // v2 se usa como operando del ADD en la posicion 2.
        CHECK(f.next_use_after(2, P(1)) == P(2), "v2 se usa en el ADD @2");
    }

    // -----------------------------------------------------------------------
    // (3) func_ptr de CALLIND cuenta como uso (igual que en liveness).
    //   pos 0: v1 = const addr
    //   pos 1: v2 = callind func_ptr=v1     (usa v1@1)
    // -----------------------------------------------------------------------
    std::printf("\n[func_ptr de CALLIND como uso]\n");
    {
        IrFunction fn;
        fn.values.resize(3);
        fn.blocks.resize(1);
        fn.blocks[0].id = 0;
        IrInstr call = mk(IrOp::CALLIND, 2);
        call.func_ptr = 1;
        fn.blocks[0].instrs = {mk(IrOp::CONST, 1, {}, 0x1000), call};

        const UseDefFacts f = analysis::compute_use_def(fn);
        CHECK(f.has_uses(1), "v1 (func_ptr) deberia contar como usado");
        CHECK(f.next_use_after(1, P(0)) == P(1), "el func_ptr v1 se usa en el CALLIND @1");
    }

    // -----------------------------------------------------------------------
    // (4) query system: query<UseDefFacts>() == compute_use_def + cache.
    // -----------------------------------------------------------------------
    std::printf("\n[query system: query<UseDefFacts>() == compute]\n");
    {
        IrFunction fn;
        fn.values.resize(3);
        fn.blocks.resize(1);
        fn.blocks[0].id = 0;
        fn.blocks[0].instrs = {
            mk(IrOp::CONST, 1, {}, 9),
            mk(IrOp::CONST, 2, {}, 3),
            mk(IrOp::ADD, IR_NO_VALUE, {1, 2}),
        };

        codegen::rbank::FunctionSnapshot s;
        s.fn = &fn;
        const UseDefFacts &q = s.use_def_facts(); // azucar de query<UseDefFacts>()
        CHECK(q.next_use_after(1, P(0)) == P(2) && q.next_use_after(2, P(0)) == P(2),
              "query<UseDefFacts> no coincide con compute_use_def");
        CHECK(s.is_computed(codegen::rbank::Fact::UseDef),
              "el Fact UseDef no quedo materializado tras la query");
        // Segunda consulta = cache hit (mismo objeto).
        CHECK(&s.use_def_facts() == &q, "la celda LazyFact no cacheo el UseDefFacts");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
