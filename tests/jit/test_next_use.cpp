/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_next_use.cpp
 * @brief MachineNextUseFacts (nivel MachineIR): next-use por vreg en la numeracion
 *        de build_intervals (2 por instr, uso en 2*gi).  Valida (1) next-use lineal
 *        + sentinela de vreg muerto + distancia, (2) numeracion CONTINUA cross-block
 *        (gi global, no reinicia por bloque), y (3) el tipo fuerte codegen::LinearPos.
 *        Construye MFunctions de juguete a mano, como test_interval.
 */

#include "jit/interval.h"

#include <cstdio>

using namespace jit;

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

static MOperand imm(int32_t v) { return MOperand::make_imm32(v); }

/// Azucar: envuelve un uint32_t crudo como posicion del dominio MachineIR.
static constexpr codegen::LinearPos P(uint32_t v) { return codegen::LinearPos{v}; }

int main() {
    std::printf("=== test_next_use (next-use MachineIR, codegen::LinearPos) ===\n");

    // -----------------------------------------------------------------------
    // (1) Straight-line.  Numeracion: instr i -> use@2i, def@2i+1.
    //   0: mov v0, 5            def v0 @1
    //   1: mov v1, 7            def v1 @3
    //   2: add v2, v0, v1       usa v0@4, v1@4; def v2 @5
    //   3: mov v3, v2           usa v2@6;       def v3 @7 (nunca usado)
    //   4: ret
    // uses: v0={4} v1={4} v2={6} v3={}
    // -----------------------------------------------------------------------
    std::printf("\n[straight-line + vreg muerto]\n");
    {
        MFunction mf;
        mf.name = "t1";
        MOperand v0 = mf.new_vreg(RegClass::GP);
        MOperand v1 = mf.new_vreg(RegClass::GP);
        MOperand v2 = mf.new_vreg(RegClass::GP);
        MOperand v3 = mf.new_vreg(RegClass::GP);
        MBlock b;
        b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(5)));
        b.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, imm(7)));
        b.instrs.push_back(MInstr::make_binary(MOp::ADD, v2, v0, v1));
        b.instrs.push_back(MInstr::make_unary(MOp::MOV, v3, v2));
        b.instrs.push_back(MInstr::make_ret());
        mf.blocks.push_back(std::move(b));

        const MachineNextUseFacts f = compute_next_use(mf);
        CHECK(f.max_pos == 10, "max_pos = 2*5 = 10");
        CHECK(f.num_vregs() == 4, "4 vregs");

        // v0: usado en el ADD @4.
        CHECK(f.next_use_after(0, P(1)) == P(4), "v0 next-use tras def@1 = 4");
        CHECK(f.next_use_after(0, P(4)) == MachineNextUseFacts::NO_NEXT_USE,
              "v0 sin uso tras 4 (muerto -> victima ideal)");
        CHECK(f.distance_to_next_use(0, P(1)) == 3u, "v0 distancia 4-1 = 3");
        // v2: usado en el MOV @6.
        CHECK(f.next_use_after(2, P(5)) == P(6), "v2 next-use tras def@5 = 6");
        // v3: definido pero NUNCA usado.
        CHECK(!f.has_uses(3), "v3 no tiene usos");
        CHECK(f.next_use_after(3, P(7)) == MachineNextUseFacts::NO_NEXT_USE,
              "v3 sin uso -> NO_NEXT_USE");
        CHECK(f.distance_to_next_use(3, P(7)) == 0xFFFFFFFFu,
              "v3 muerto -> distancia infinita");
        // Belady: en el punto del ADD (pos 4), v0/v1 mueren enseguida (mejor
        // victima) que v2 (que se reusa en 6).
        CHECK(f.has_uses(0) && f.has_uses(1) && f.has_uses(2), "v0/v1/v2 usados");
        // Fuera de rango (defensivo).
        CHECK(f.next_use_after(99, P(0)) == MachineNextUseFacts::NO_NEXT_USE,
              "vreg fuera de rango -> NO_NEXT_USE (sin crash)");
    }

    // -----------------------------------------------------------------------
    // (2) Cross-block: la numeracion es CONTINUA (gi global), no reinicia por
    //     bloque.  v0 def en b0, usado en b1.
    //   b0  0: mov v0, 5        def v0 @1
    //   b1  1: mov v1, v0       usa v0@2; def v1 @3
    //       2: ret
    //   -> v0 se usa en use_pos GLOBAL 2 (no en 0).
    // -----------------------------------------------------------------------
    std::printf("\n[cross-block: numeracion continua]\n");
    {
        MFunction mf;
        mf.name = "t2";
        MOperand v0 = mf.new_vreg(RegClass::GP);
        MOperand v1 = mf.new_vreg(RegClass::GP);
        MBlock b0;
        b0.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(5)));
        MBlock b1;
        b1.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, v0));
        b1.instrs.push_back(MInstr::make_ret());
        mf.blocks.push_back(std::move(b0));
        mf.blocks.push_back(std::move(b1));

        const MachineNextUseFacts f = compute_next_use(mf);
        CHECK(f.max_pos == 6, "max_pos = 2*3 = 6");
        // v0 usado en la 2a instr global (gi=1) -> use_pos 2.
        CHECK(f.next_use_after(0, P(1)) == P(2),
              "v0 usado en b1 -> use_pos GLOBAL 2 (no reinicia por bloque)");
        CHECK(f.distance_to_next_use(0, P(1)) == 1u, "v0 distancia 2-1 = 1");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
