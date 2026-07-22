/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_timeline_builder.cpp
 * @brief Splitting incremento 1: el @c TimelineBuilder trivial (SplitPlan vacio) produce
 *        un @c AllocationTimeline EQUIVALENTE a la @c RegAlloc plana.  Es la garantia que
 *        permitira al Rewrite consumir el timeline sin cambiar el codigo emitido antes de
 *        que exista un split real: en TODA posicion viva, at(pos) == ra.assign[vreg].
 */

#include "codegen/timeline_builder.h"
#include "jit/interval.h"

#include <cstdio>

using namespace codegen;

static int g_checks = 0, g_fails = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(c)) {                                                              \
            ++g_fails;                                                           \
            std::printf("  FALLO L%d: %s\n", __LINE__, #c);                      \
        }                                                                        \
    } while (0)

/// LiveInterval minimo: un vreg con un unico rango [from, to) (o muerto si to<=from).
static jit::LiveInterval mk(uint32_t vreg, uint32_t from, uint32_t to) {
    jit::LiveInterval li;
    li.vreg = vreg;
    if (to > from) li.ranges.push_back({from, to});
    return li;
}

int main() {
    std::printf("=== test_timeline_builder (Splitting incremento 1) ===\n");

    // 3 vregs: 0 en REG r5 [10,20), 1 en SPILL slot 2 [4,30), 2 muerto.
    jit::IntervalResult ivs;
    ivs.intervals.push_back(mk(0, 10, 20));
    ivs.intervals.push_back(mk(1, 4, 30));
    ivs.intervals.push_back(mk(2, 0, 0)); // muerto (sin rangos).

    RegAlloc ra;
    ra.assign.resize(3);
    ra.assign[0] = {RegAlloc::Loc::REG, /*reg*/ 5, /*slot*/ 0};
    ra.assign[1] = {RegAlloc::Loc::SPILL, 0, /*slot*/ 2};
    ra.assign[2] = {RegAlloc::Loc::NONE, 0, 0};

    SplitPlan plan; // vacio -> caso trivial (equivalente a RegAlloc).
    const AllocationResult ar = build_allocation_result(ra, &ivs, plan);
    const AllocationTimeline &tl = ar.timeline;

    CHECK(tl.values.size() == 3);

    // El Rewrite usa SOLO tl.lookup(vreg, pos) (no segments): vreg 0 en REG r5 [10,20).
    CHECK(tl.lookup(0, LinearPos{9}) == nullptr);  // antes del rango.
    const Location *l0 = tl.lookup(0, LinearPos{15});
    CHECK(l0 && l0->loc == RegAlloc::Loc::REG && l0->reg == 5);
    CHECK(tl.lookup(0, LinearPos{20}) == nullptr); // 'to' es exclusive.

    // vreg 1: SPILL slot 2 en [4,30).
    const Location *l1 = tl.lookup(1, LinearPos{4});
    CHECK(l1 && l1->loc == RegAlloc::Loc::SPILL && l1->slot == 2);
    CHECK(tl.lookup(1, LinearPos{29}) != nullptr);
    CHECK(tl.lookup(1, LinearPos{30}) == nullptr);

    // vreg 2: muerto -> sin ubicacion en ninguna posicion.
    CHECK(tl.lookup(2, LinearPos{0}) == nullptr);

    // FrameLayout: viene de la RegAlloc, separado del timeline (no temporal).
    CHECK(ar.frame.num_spill_slots == ra.num_spill_slots);
    CHECK(ar.frame.callee_saved_used == ra.callee_saved_used);

    // EQUIVALENCIA CON RegAlloc: en toda posicion viva de cada vreg, lookup coincide
    // EXACTAMENTE con la asignacion plana.
    for (uint32_t v = 0; v < ivs.intervals.size(); ++v)
        for (const jit::LiveRange &r : ivs.intervals[v].ranges)
            for (uint32_t p = r.from; p < r.to; ++p) {
                const Location *loc = tl.lookup(v, LinearPos{p});
                CHECK(loc && loc->loc == ra.assign[v].loc &&
                      loc->reg == ra.assign[v].reg && loc->slot == ra.assign[v].slot);
            }

    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
