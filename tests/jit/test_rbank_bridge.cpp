/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_rbank_bridge.cpp
 * @brief F5 (backend bridge): traduccion MECANICA LaneAssignment <-> jit::RegAlloc.
 *        El puente debe ser tonto y fiel: lane->reg, spill->slot, callee_saved_used
 *        correcto, denso 0..vreg_count-1.  Property: round-trip preserva la ubicacion
 *        (REG/SPILL) y el reg de cada valor.
 */

#include "codegen/rbank/backend_bridge.h"
#include "codegen/rbank/physical_bank.h"

#include <cstdio>
#include <random>

using namespace jit;            // BackendCaps, RegAlloc.
using namespace codegen::rbank; // el modelo + el puente.

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

static AbstractValue mkval(uint32_t id, uint32_t start, uint32_t end) {
    AbstractValue v;
    v.value_id = id;
    v.start = start;
    v.end = end;
    v.req.value_id = id;
    v.req.cls = ResourceClass::GP;
    v.req.width = ViewWidth::W8;
    return v;
}

int main() {
    std::printf("=== test_rbank_bridge (F5: LaneAssignment <-> jit::RegAlloc) ===\n");

    const BackendCaps caps = [] { BackendCaps c{}; c.sse2 = true; return c; }();
    PhysicalRegisterBank bank = physical_bank_x86_64(true, caps); // SysV.

    // Elige un callee-saved y un caller-saved reales del banco para el test.
    int callee_id = -1, caller_id = -1;
    for (const Lane &l : bank.lanes) {
        if (l.cls != ResourceClass::GP || !l.allocatable(false)) continue;
        if (l.preservation_of(ViewWidth::W8) == SavePolicy::PRESERVED && callee_id < 0)
            callee_id = l.id;
        if (l.preservation_of(ViewWidth::W8) == SavePolicy::VOLATILE && caller_id < 0)
            caller_id = l.id;
    }
    CHECK(callee_id >= 0 && caller_id >= 0, "banco sin callee/caller-saved GP?");

    // --- from_lanes: lane->reg, spill->slot, callee_saved_used, denso ---
    std::printf("\n[regalloc_from_lanes: mapeo mecanico]\n");
    {
        AbstractProblem p;
        p.values = {mkval(0, 0, 10), mkval(1, 0, 10), mkval(3, 0, 10)}; // vreg 2 muerto.
        LaneAssignment la;
        la.assign(0, static_cast<uint8_t>(caller_id)); // caller-saved
        la.assign(1, static_cast<uint8_t>(callee_id)); // callee-saved
        la.spill(3);                                    // derramado

        RegAlloc ra = regalloc_from_lanes(la, p, bank, /*vreg_count=*/4);
        CHECK(ra.assign.size() == 4, "assign no es denso a vreg_count");
        CHECK(ra.assign[0].loc == RegAlloc::Loc::REG && ra.assign[0].reg == caller_id,
              "vreg 0 mal mapeado a caller-saved");
        CHECK(ra.assign[1].loc == RegAlloc::Loc::REG && ra.assign[1].reg == callee_id,
              "vreg 1 mal mapeado a callee-saved");
        CHECK(ra.assign[2].loc == RegAlloc::Loc::NONE, "vreg 2 (muerto) no es NONE");
        CHECK(ra.assign[3].loc == RegAlloc::Loc::SPILL, "vreg 3 no quedo SPILL");
        CHECK(ra.num_spill_slots == 1, "num_spill_slots != 1");
        // callee_saved_used: solo el callee, NO el caller.
        CHECK(ra.callee_saved_used.size() == 1 &&
              ra.callee_saved_used[0] == callee_id,
              "callee_saved_used mal (deberia ser solo el callee)");
    }

    // --- round-trip: RegAlloc -> lanes -> RegAlloc preserva ubicacion + reg ---
    std::printf("\n[round-trip preserva REG/SPILL + reg]\n");
    {
        std::mt19937 rng(0xB21D6Eu);
        int ok = 0;
        const int TRIALS = 500;
        // ids GP allocatable del banco (para asignaciones aleatorias validas).
        std::vector<uint8_t> gp;
        for (const Lane &l : bank.lanes)
            if (l.cls == ResourceClass::GP && l.allocatable(false)) gp.push_back(l.id);

        for (int t = 0; t < TRIALS; ++t) {
            std::uniform_int_distribution<uint32_t> nvals(1, 12);
            std::uniform_int_distribution<size_t> pick(0, gp.size() - 1);
            std::bernoulli_distribution sp(0.3);
            const uint32_t n = nvals(rng);

            AbstractProblem p;
            RegAlloc orig;
            orig.assign.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                p.values.push_back(mkval(i, 0, 10));
                if (sp(rng)) {
                    orig.assign[i] = {RegAlloc::Loc::SPILL, 0, i};
                } else {
                    orig.assign[i] = {RegAlloc::Loc::REG, gp[pick(rng)], 0};
                }
            }

            LaneAssignment la = regalloc_to_lanes(orig, p);
            RegAlloc rt = regalloc_from_lanes(la, p, bank, n);

            bool same = true;
            for (uint32_t i = 0; i < n; ++i) {
                if (orig.assign[i].loc != rt.assign[i].loc) same = false;
                if (orig.assign[i].loc == RegAlloc::Loc::REG &&
                    orig.assign[i].reg != rt.assign[i].reg) same = false;
            }
            if (same) ++ok;
        }
        CHECK(ok == TRIALS, "round-trip no preservo ubicacion/reg en algun caso");
        std::printf("  round-trip fiel: %d/%d\n", ok, TRIALS);
    }

    std::printf("\n=== %d checks, %d fallos ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
