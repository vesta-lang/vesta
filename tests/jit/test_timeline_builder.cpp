/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_timeline_builder.cpp
 * @brief Splitting incremento 1: el @c TimelineBuilder trivial (AssignmentPlan vacio) produce
 *        un @c AllocationTimeline EQUIVALENTE a la @c RegAlloc plana.  Es la garantia que
 *        permitira al Rewrite consumir el timeline sin cambiar el codigo emitido antes de
 *        que exista un split real: en TODA posicion viva, at(pos) == ra.assign[vreg].
 */

#include "codegen/timeline_builder.h"
#include "codegen/transition_planner.h"
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

    AssignmentPlan plan; // vacio -> caso trivial (equivalente a RegAlloc).
    const AllocationResult ar = build_allocation_result(ra, &ivs, plan);
    const AllocationTimeline &tl = ar.timeline;

    CHECK(tl.values.size() == 3);

    // El Rewrite usa SOLO tl.lookup(vreg,pos) -> ValueLocation (ni segments ni enum):
    // vreg 0 en REG r5 [10,20).
    CHECK(tl.lookup(0, LinearPos{9}).is_none());  // antes del rango.
    const ValueLocation l0 = tl.lookup(0, LinearPos{15});
    CHECK(l0.is_register() && l0.register_id() == 5);
    CHECK(tl.lookup(0, LinearPos{20}).is_none()); // 'to' es exclusive.

    // vreg 1: memoria (slot 2) en [4,30).
    const ValueLocation l1 = tl.lookup(1, LinearPos{4});
    CHECK(l1.is_memory() && l1.stack_slot() == 2);
    CHECK(!tl.lookup(1, LinearPos{29}).is_none());
    CHECK(tl.lookup(1, LinearPos{30}).is_none());

    // vreg 2: muerto -> sin ubicacion en ninguna posicion.
    CHECK(tl.lookup(2, LinearPos{0}).is_none());

    // FrameLayout: viene de la RegAlloc, separado del timeline (no temporal).
    CHECK(ar.frame.num_spill_slots == ra.num_spill_slots);
    CHECK(ar.frame.callee_saved_used == ra.callee_saved_used);

    // El timeline trivial es SEMANTICAMENTE equivalente a la asignacion plana: en toda
    // posicion viva devuelve la misma ubicacion.  Se compara COMPORTAMIENTO, no estructuras.
    for (uint32_t v = 0; v < ivs.intervals.size(); ++v)
        for (const jit::LiveRange &r : ivs.intervals[v].ranges)
            for (uint32_t p = r.from; p < r.to; ++p) {
                const ValueLocation loc = tl.lookup(v, LinearPos{p});
                const auto &va = ra.assign[v];
                if (va.loc == RegAlloc::Loc::REG)
                    CHECK(loc.is_register() && loc.register_id() == va.reg);
                else
                    CHECK(loc.is_memory() && loc.stack_slot() == va.slot);
            }

    /* --- El builder MATERIALIZA las afirmaciones del plan ---
     * vreg 1 vive en memoria (slot 2) en [4,30); el plan AFIRMA que en [10,20) vive en r7.
     * Se comprueba el CONTRATO ("que ubicacion da cada posicion"), NUNCA la representacion
     * interna: cuantos segmentos use el builder es cosa suya (manana podria fusionarlos,
     * usar un arbol o una tabla y seguir siendo correcto). */
    AssignmentPlan sp;
    sp.add(1, LinearPos{10}, LinearPos{20}, ValueLocation{ValueLocation::Register{7}});
    const AllocationResult ar2 = build_allocation_result(ra, &ivs, sp);
    const AllocationTimeline &tl2 = ar2.timeline;

    // CONTINUIDAD: barrer TODA la vida, no puntos sueltos.  Un borde corrido (un REG de
    // mas o de menos en la frontera) se escaparia comprobando solo los extremos.
    for (uint32_t p = 4; p < 10; ++p)
        CHECK(tl2.lookup(1, LinearPos{p}).is_memory() &&
              tl2.lookup(1, LinearPos{p}).stack_slot() == 2);
    for (uint32_t p = 10; p < 20; ++p) // inicio inclusive, fin exclusive.
        CHECK(tl2.lookup(1, LinearPos{p}).is_register() &&
              tl2.lookup(1, LinearPos{p}).register_id() == 7);
    for (uint32_t p = 20; p < 30; ++p)
        CHECK(tl2.lookup(1, LinearPos{p}).is_memory() &&
              tl2.lookup(1, LinearPos{p}).stack_slot() == 2);
    CHECK(tl2.lookup(1, LinearPos{3}).is_none());  // antes de la vida.
    CHECK(tl2.lookup(1, LinearPos{30}).is_none()); // fuera de la vida.
    // Un vreg NO mencionado por el plan queda intacto.
    CHECK(tl2.lookup(0, LinearPos{15}).is_register() &&
          tl2.lookup(0, LinearPos{15}).register_id() == 5);

    /* --- Una afirmacion cuyo tramo cae FUERA de la vida del valor no cambia nada ---
     * (caso barato que caza errores de limites: el intervalo no debe tocar ni inventar). */
    AssignmentPlan out_of_range;
    out_of_range.add(1, LinearPos{100}, LinearPos{120},
                     ValueLocation{ValueLocation::Register{9}});
    const AllocationResult ar3 = build_allocation_result(ra, &ivs, out_of_range);
    const AllocationTimeline &tl3 = ar3.timeline;
    for (uint32_t p = 4; p < 30; ++p) // toda la vida intacta en memoria.
        CHECK(tl3.lookup(1, LinearPos{p}).is_memory() &&
              tl3.lookup(1, LinearPos{p}).stack_slot() == 2);
    CHECK(tl3.lookup(1, LinearPos{110}).is_none()); // el tramo no inventa vida.

    /* --- TransitionPlanner: "¿que movimientos exige este punto del programa?" ---
     * Se prueba AISLADO (sin ejecutar el Rewrite).  Sobre el timeline MEM|REG|MEM del vreg
     * 1 debe exigir exactamente DOS movimientos, en program points, no en posiciones:
     *   antes de la instr 5  (donde empieza el tramo en registro):  MEM  -> REG r7
     *   antes de la instr 10 (donde vuelve a memoria):              REG  -> MEM slot 2 */
    const TransitionPlanner planner(tl2);
    CHECK(!planner.empty());

    const auto &in5 = planner.before_instruction(5);
    CHECK(in5.size() == 1);
    if (in5.size() == 1) {
        CHECK(in5[0].vreg == 1);
        CHECK(in5[0].from.is_memory() && in5[0].from.stack_slot() == 2);
        CHECK(in5[0].to.is_register() && in5[0].to.register_id() == 7);
    }
    const auto &in10 = planner.before_instruction(10);
    CHECK(in10.size() == 1);
    if (in10.size() == 1) {
        CHECK(in10[0].vreg == 1);
        CHECK(in10[0].from.is_register() && in10[0].from.register_id() == 7);
        CHECK(in10[0].to.is_memory() && in10[0].to.stack_slot() == 2);
    }
    // En cualquier otro punto NO se exige nada (ni dentro del tramo ni fuera de la vida).
    for (uint32_t gi = 0; gi < 20; ++gi) {
        if (gi == 5 || gi == 10) continue;
        CHECK(planner.before_instruction(gi).empty());
        CHECK(planner.after_instruction(gi).empty());
    }

    /* Sin afirmaciones (timeline trivial) NO hay ningun movimiento: por eso el codigo
     * emitido no cambia mientras nadie produzca planes. */
    const TransitionPlanner planner_trivial(tl);
    CHECK(planner_trivial.empty());
    for (uint32_t gi = 0; gi < 20; ++gi)
        CHECK(planner_trivial.before_instruction(gi).empty());

    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
