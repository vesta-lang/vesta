/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_constraints.cpp
 * @brief Test de @c ConstraintSet + @c validate_assignment (nivel 2, banco
 *        ancho): el verificador de soluciones.  Comprueba interferencia (con
 *        aliasing), pines, must-differ/same y satisfaccion de
 * ValueRequirements, todo devolviendo DATOS i18n-ready.
 */

#include "codegen/rbank/constraints.h"
#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/value_requirements.h"

#include <cstdio>

using namespace jit;
using namespace codegen::rbank;

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

// ids de lanes x86-64: RAX=0 RCX=1 RDX=2; XMM0=16 XMM1=17 XMM14=30 (scratch).
enum { RAX = 0, RCX = 1, RDX = 2, XMM0 = 16, XMM1 = 17, XMM14 = 30 };

int main() {
    std::printf("=== test_rbank_constraints (Fase 0: verificador) ===\n");

    const BackendCaps avx2 = [] {
        BackendCaps c{};
        c.sse2 = c.avx = c.avx2 = true;
        return c;
    }();
    PhysicalRegisterBank bank = physical_bank_x86_64(true, avx2);
    const std::vector<ValueRequirements> no_reqs;

    auto vk = [](const ConstraintViolation &v) { return v.kind; };
    auto ok = [](const ConstraintViolation &v) { return v.ok; };

    // --- INTERFERE ---
    std::printf("\n[interferencia]\n");
    {
        ConstraintSet cs;
        cs.interfere(1, 2);
        LaneAssignment a;
        a.assign(1, RAX);
        a.assign(2, RAX); // misma lane
        auto v = validate_assignment(cs, a, no_reqs, bank, false);
        CHECK(!ok(v) && vk(v) == ViolationKind::INTERFERENCE_OVERLAP,
              "misma lane con interferencia no viola");
    }
    {
        ConstraintSet cs;
        cs.interfere(1, 2);
        LaneAssignment a;
        a.assign(1, RAX);
        a.assign(2, RCX); // lanes distintas
        CHECK(ok(validate_assignment(cs, a, no_reqs, bank, false)),
              "lanes distintas con interferencia viola indebidamente");
    }
    {
        ConstraintSet cs;
        cs.interfere(1, 2);
        LaneAssignment a;
        a.spill(1);
        a.assign(2, RAX); // uno spilled
        CHECK(ok(validate_assignment(cs, a, no_reqs, bank, false)),
              "spilled interfiere indebidamente");
    }
    {
        ConstraintSet cs;
        cs.interfere(1, 2);
        CHECK(cs.interferes(1, 2) && cs.interferes(2, 1) &&
                  !cs.interferes(1, 3),
              "query interferes() incorrecta");
    }

    // --- DIFFERENT_LANE / SAME_LANE ---
    std::printf("\n[must-differ / must-same]\n");
    {
        ConstraintSet cs;
        cs.different_lane(1, 2);
        LaneAssignment a;
        a.assign(1, RAX);
        a.assign(2, RAX);
        auto v = validate_assignment(cs, a, no_reqs, bank, false);
        CHECK(!ok(v) && vk(v) == ViolationKind::DIFFERENT_LANE_VIOLATED,
              "different_lane con misma lane no viola");
    }
    {
        ConstraintSet cs;
        cs.same_lane(1, 2);
        LaneAssignment a;
        a.assign(1, RAX);
        a.assign(2, RCX);
        auto v = validate_assignment(cs, a, no_reqs, bank, false);
        CHECK(!ok(v) && vk(v) == ViolationKind::SAME_LANE_VIOLATED,
              "same_lane con lanes distintas no viola");
    }
    {
        ConstraintSet cs;
        cs.same_lane(1, 2);
        LaneAssignment a;
        a.assign(1, RDX);
        a.assign(2, RDX);
        CHECK(ok(validate_assignment(cs, a, no_reqs, bank, false)),
              "same_lane con misma lane viola indebidamente");
    }

    // --- FIXED_LANE ---
    std::printf("\n[pin fijo]\n");
    {
        ConstraintSet cs;
        cs.fixed_lane(1, RCX);
        LaneAssignment a;
        a.assign(1, RAX);
        auto v = validate_assignment(cs, a, no_reqs, bank, false);
        CHECK(!ok(v) && vk(v) == ViolationKind::FIXED_LANE_VIOLATED,
              "fixed_lane en lane equivocada no viola");
    }
    {
        ConstraintSet cs;
        cs.fixed_lane(1, RCX);
        LaneAssignment a;
        a.assign(1, RCX);
        CHECK(ok(validate_assignment(cs, a, no_reqs, bank, false)),
              "fixed_lane en lane correcta viola indebidamente");
    }

    // --- REQUIREMENT_UNSAT (la lane no satisface el requisito del valor) ---
    std::printf("\n[requisito del valor sobre la lane asignada]\n");
    {
        // valor GP asignado a XMM0 (FP) -> WRONG_CLASS.
        std::vector<ValueRequirements> reqs(1);
        reqs[0].value_id = 1;
        reqs[0].cls = ResourceClass::GP;
        reqs[0].width = ViewWidth::W8;
        LaneAssignment a;
        a.assign(1, XMM0);
        auto v = validate_assignment({}, a, reqs, bank, false);
        CHECK(!ok(v) && vk(v) == ViolationKind::REQUIREMENT_UNSAT &&
                  v.unsat == UnsatReason::FIXED_REG_WRONG_CLASS,
              "GP en lane FP no da REQUIREMENT_UNSAT/WRONG_CLASS");
    }
    {
        // valor FP W64 (ZMM) en XMM0 con avx2 -> el ancho no cabe.
        std::vector<ValueRequirements> reqs(1);
        reqs[0].value_id = 1;
        reqs[0].cls = ResourceClass::FP_VECTOR;
        reqs[0].width = ViewWidth::W64;
        LaneAssignment a;
        a.assign(1, XMM0);
        auto v = validate_assignment({}, a, reqs, bank, false);
        CHECK(!ok(v) && v.unsat == UnsatReason::FIXED_REG_WIDTH_UNSUPPORTED,
              "FP W64 en XMM0/avx2 no da WIDTH_UNSUPPORTED");
    }
    {
        // valor FP asignado a XMM14 (scratch) -> no puede alojar valor.
        std::vector<ValueRequirements> reqs(1);
        reqs[0].value_id = 1;
        reqs[0].cls = ResourceClass::FP_VECTOR;
        reqs[0].width = ViewWidth::W8;
        LaneAssignment a;
        a.assign(1, XMM14);
        auto v = validate_assignment({}, a, reqs, bank, false);
        CHECK(!ok(v) && v.unsat == UnsatReason::FIXED_REG_UNUSABLE,
              "FP en scratch XMM14 no da UNUSABLE");
    }

    // --- Asignacion valida completa ---
    std::printf("\n[solucion valida completa]\n");
    {
        std::vector<ValueRequirements> reqs(2);
        reqs[0].value_id = 1;
        reqs[0].cls = ResourceClass::GP;
        reqs[0].width = ViewWidth::W8;
        reqs[1].value_id = 2;
        reqs[1].cls = ResourceClass::FP_VECTOR;
        reqs[1].width = ViewWidth::W8;
        ConstraintSet cs;
        cs.interfere(1, 2); // clases distintas, lanes disjuntas.
        LaneAssignment a;
        a.assign(1, RAX);
        a.assign(2, XMM0);
        CHECK(ok(validate_assignment(cs, a, reqs, bank, false)),
              "solucion valida marcada como invalida");
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
