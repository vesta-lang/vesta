/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_const_adapter.cpp
 * @brief Test del ConstAdapter (Fase 0.25): is_const -> rematerializable.
 */

#include "ir/ssa_ir.h"
#include "codegen/rbank/adapters/const_adapter.h"
#include "codegen/rbank/value_requirements.h"

#include <cstdio>

using namespace jit;
using namespace codegen::rbank;

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

int main() {
    std::printf("=== test_rbank_const_adapter (Fase 0.25.3) ===\n");

    std::printf("\n[is_const -> rematerializable]\n");
    {
        ir::IrValue v; v.id = 0; v.type = ir::IrType::I64; v.is_const = true;
        ValueRequirements r;
        populate_const_requirements(r, v);
        CHECK(r.rematerializable, "const no marca rematerializable");
    }
    {
        ir::IrValue v; v.id = 1; v.type = ir::IrType::I64; v.is_const = false;
        ValueRequirements r;
        populate_const_requirements(r, v);
        CHECK(!r.rematerializable, "no-const marca rematerializable");
    }

    std::printf("\n[solo toca rematerializable]\n");
    {
        ir::IrValue v; v.is_const = true;
        ValueRequirements r;
        r.value_id = 9; r.crosses_call = true; r.is_gc = true; r.loop_depth = 3;
        r.cls = ResourceClass::FP_VECTOR; r.width = ViewWidth::W8;
        populate_const_requirements(r, v);
        CHECK(r.rematerializable, "no actualizo rematerializable");
        CHECK(r.value_id == 9 && r.crosses_call && r.is_gc && r.loop_depth == 3 &&
              r.cls == ResourceClass::FP_VECTOR && r.width == ViewWidth::W8,
              "el ConstAdapter toco campos ajenos");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
