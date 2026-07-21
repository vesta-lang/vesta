/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_type_adapter.cpp
 * @brief Test del TypeAdapter (Fase 0.25): IrType -> clase + width.  Verifica la
 *        traduccion pura para TODOS los IrType, la consistencia con la fuente
 *        canonica (memory_access_size) y que el adaptador SOLO toca cls/width.
 */

#include "analysis/memory/memory_access.h"
#include "ir/ssa_ir.h"
#include "codegen/rbank/adapters/type_adapter.h"
#include "codegen/rbank/value_requirements.h"

#include <cstdio>

using namespace jit;
using namespace jit::rbank;
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

static void expect(IrType t, ResourceClass cls, ViewWidth w, const char *name) {
    ValueRequirements r;
    populate_type_requirements(r, t);
    if (r.cls != cls || r.width != w) {
        ++g_fail;
        std::printf("  [FAIL] %s: cls/width inesperados\n", name);
    }
    ++g_checks;
    // Consistencia con la fuente canonica de tamano.
    ++g_checks;
    if (static_cast<uint16_t>(r.width) <
        static_cast<uint16_t>(view_width_for_bytes(
            static_cast<uint32_t>(analysis::memory_access_size(t))))) {
        ++g_fail;
        std::printf("  [FAIL] %s: width no consistente con memory_access_size\n", name);
    }
}

int main() {
    std::printf("=== test_rbank_type_adapter (Fase 0.25) ===\n");

    std::printf("\n[IrType -> clase + width]\n");
    expect(IrType::I8,     ResourceClass::GP,        ViewWidth::W1, "i8");
    expect(IrType::U8,     ResourceClass::GP,        ViewWidth::W1, "u8");
    expect(IrType::BOOL,   ResourceClass::GP,        ViewWidth::W1, "bool");
    expect(IrType::I16,    ResourceClass::GP,        ViewWidth::W2, "i16");
    expect(IrType::U16,    ResourceClass::GP,        ViewWidth::W2, "u16");
    expect(IrType::I32,    ResourceClass::GP,        ViewWidth::W4, "i32");
    expect(IrType::U32,    ResourceClass::GP,        ViewWidth::W4, "u32");
    expect(IrType::I64,    ResourceClass::GP,        ViewWidth::W8, "i64");
    expect(IrType::U64,    ResourceClass::GP,        ViewWidth::W8, "u64");
    expect(IrType::PTR,    ResourceClass::GP,        ViewWidth::W8, "ptr");
    expect(IrType::HANDLE, ResourceClass::GP,        ViewWidth::W4, "handle");
    expect(IrType::F32,    ResourceClass::FP_VECTOR, ViewWidth::W4, "f32");
    expect(IrType::F64,    ResourceClass::FP_VECTOR, ViewWidth::W8, "f64");

    std::printf("\n[contrato view_width_for_bytes: redondea ARRIBA, sin fallos]\n");
    CHECK(view_width_for_bytes(0) == ViewWidth::W1, "0B != W1");
    CHECK(view_width_for_bytes(3) == ViewWidth::W4, "3B no redondea a W4");
    CHECK(view_width_for_bytes(5) == ViewWidth::W8, "5B no redondea a W8");
    CHECK(view_width_for_bytes(9) == ViewWidth::W16, "9B no redondea a W16");
    CHECK(view_width_for_bytes(1000) == ViewWidth::W64, ">64B no satura a W64");

    std::printf("\n[type_has_value: VOID no produce valor]\n");
    CHECK(!type_has_value(IrType::VOID), "VOID produce valor");
    CHECK(type_has_value(IrType::I64) && type_has_value(IrType::F64),
          "tipo con valor marcado sin valor");

    std::printf("\n[el adaptador SOLO toca cls/width]\n");
    {
        ValueRequirements r;
        r.value_id       = 42;
        r.crosses_call   = true;
        r.is_gc          = true;
        r.address_taken  = true;
        r.rematerializable = true;
        r.loop_depth     = 5;
        r.fixed_reg      = 7;
        r.residency      = Residency::MEMORY;
        populate_type_requirements(r, IrType::F64);
        // cls/width actualizados...
        CHECK(r.cls == ResourceClass::FP_VECTOR && r.width == ViewWidth::W8,
              "no actualizo cls/width");
        // ...y NADA mas cambiado (esos hechos son de otros adaptadores).
        CHECK(r.value_id == 42 && r.crosses_call && r.is_gc && r.address_taken &&
              r.rematerializable && r.loop_depth == 5 && r.fixed_reg == 7 &&
              r.residency == Residency::MEMORY,
              "el TypeAdapter toco campos que no le corresponden");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
