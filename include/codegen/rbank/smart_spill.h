/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/smart_spill.h
 * @brief Fase 5 (nucleo): coloreado con SPILL INTELIGENTE -- elige la victima por
 *        COSTE en vez del spill NAIVE (que derrama el valor actual sin criterio).
 *
 *     AbstractProblem + OptimizationContext  --color_smart_spill-->  LaneAssignment
 *                                                                    (menor coste de spill)
 *
 * DISCIPLINA (dato != mecanismo -- misma que FunctionSnapshot).  El @c Objective
 * DESCRIBE el coste de derramar cada valor (@c spill_cost_of: weight-aware, barato
 * lo frio/rematerializable, caro lo caliente).  Es DATO/preferencia y no sabe nada
 * de "victimas" ni de "Belady".  Este fichero es el ALGORITMO que EXPLOTA ese coste;
 * la ESTRATEGIA de eleccion de victima vive AQUI, desacoplada.  Cambiar de politica
 * de spill = otro algoritmo (u otra DecisionPolicy), NUNCA meter la politica en el
 * Objective.  Asi el Objective no se convierte en el vertedero de toda la
 * inteligencia (spill/move/affinity/sched/remat).
 *
 * ESTRATEGIA (heuristica COST-AWARE de DURACION RESTANTE -- NO es Belady).  Cuando
 * no cabe un valor, la victima MAXIMIZA  duracion_restante / coste_de_spill.  Dos
 * factores ortogonales:
 *   - duracion_restante = end - punto_actual  (ESTRUCTURA, el LiveRange).
 *   - coste_de_spill    = @c Objective (weight/remat).
 * Preferir derramar lo que ocupa la lane MUCHO tiempo y cuesta POCO.  El coste lo
 * pone el Objective; la COMBINACION es del algoritmo.
 *
 * POR QUE NO ES BELADY (importante).  Belady elige por NEXT-USE DISTANCE (el uso mas
 * lejano en el FUTURO); esto usa la DURACION RESTANTE (@c end - @c now), que NO es lo
 * mismo: un valor que vive hasta @c end=200 pero se USA en 100,101,102 es MALA
 * victima para Belady (se reusa enseguida) aunque su duracion sea larga.  El Belady
 * REAL necesita @c next_use(v), que hoy NO existe -- llegara con @c UseDefFacts (la
 * evolucion natural: F5 -> UseDefFacts -> next-use -> Belady real).  Hasta entonces
 * la duracion restante es una APROXIMACION razonable.
 *
 * LIMITES (honestos):
 *   - GREEDY: al derramar una victima no reconsidera decisiones anteriores -> optimo
 *     LOCAL, no global (el optimo global es el solver de Pareto de F5 avanzado).
 *   - Es una DecisionPolicy EMBRIONARIA: @c spill_score ES @c choose_spill_victim.
 *     Manana seran @c CostPolicy / @c BeladyPolicy / @c ParetoPolicy intercambiables
 *     sin tocar el resto del allocator (P8/P16).
 *   - CONTEXTO como FACHADA (vigilar): el @c OptimizationContext no debe volverse un
 *     God Context; a medida que lleguen next_use/loop-profile/alias, mejor que sea
 *     una fachada a Facts (ctx.usedef()/profile()/hw()/objective()) que un almacen.
 *
 * i18n: produce DATOS.  Fase 5 (nucleo): ADITIVO, sin consumidores de produccion
 * (solo el prototipo/test).
 */

#ifndef VESTA_CODEGEN_RBANK_SMART_SPILL_H
#define VESTA_CODEGEN_RBANK_SMART_SPILL_H

#include "codegen/rbank/abstract_problem.h"
#include "codegen/rbank/allowed_lanes.h"
#include "codegen/rbank/constraints.h"
#include "codegen/rbank/objective.h"
#include "codegen/rbank/optimization_context.h"
#include "codegen/rbank/physical_bank.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @brief Coste de derramar el valor @p r -- lo aporta el @c Objective (dato).  El
 *        algoritmo de spill NO lo define, solo lo consulta.
 */
inline double spill_cost_via_objective(const ValueRequirements &r,
                                       const OptimizationContext &ctx) {
    return spill_cost_of(r, ctx.execution_weight(r), ctx.hw());
}

/**
 * @brief Colorea el problema con SPILL INTELIGENTE (heuristica cost-aware de
 *        duracion restante; NO Belady -- ese necesita next-use/UseDefFacts).  El
 *        coste de cada candidato lo da el Objective (via @p ctx); la eleccion de
 *        victima es la estrategia de este algoritmo.
 */
inline LaneAssignment color_smart_spill(const AbstractProblem &p,
                                        const OptimizationContext &ctx,
                                        bool vec_active) {
    const PhysicalRegisterBank &bank = ctx.get_bank();
    LaneAssignment out;

    // Barrido por punto de definicion (desempate por value_id, determinista).
    std::vector<const AbstractValue *> order;
    order.reserve(p.values.size());
    for (const AbstractValue &v : p.values) order.push_back(&v);
    std::sort(order.begin(), order.end(),
              [](const AbstractValue *a, const AbstractValue *b) {
                  if (a->start != b->start) return a->start < b->start;
                  return a->value_id < b->value_id;
              });

    // FIXED INTERVALS (linear-scan clasico): un valor con @c fixed_reg reserva ESA
    // lane durante todo su rango.  El barrido es por @c start, asi que un valor SIN
    // pin definido ANTES podria robar una lane que un pin definido DESPUES (aun no
    // activo) necesita -> el pin, al llegar, no la encuentra y spillea/se desvia
    // (rompe register("rXX") con varios bindings, p.ej. syscalls).  Se pre-computa
    // aqui la reserva de cada pin para que @c first_free_lane la respete SIEMPRE.
    struct FixedIval { uint8_t fid; uint32_t start; uint32_t end; uint32_t vid; };
    std::vector<FixedIval> fixed_ivals;
    for (const AbstractValue &v : p.values)
        if (v.req.fixed_reg >= 0)
            fixed_ivals.push_back({static_cast<uint8_t>(v.req.fixed_reg), v.start,
                                   v.end, v.value_id});
    // ¿La lane @p id la reserva ALGUN pin (distinto de @p self) cuyo rango se
    // solapa con [@p vs, @p ve]?  El aliasing sub-registro se captura via AliasSet.
    auto lane_pinned_by_other = [&](uint8_t id, uint32_t vs, uint32_t ve,
                                    uint32_t self) -> bool {
        const AliasSet *ca = bank.aliases_of(id);
        if (!ca) return false;
        for (const FixedIval &fi : fixed_ivals) {
            if (fi.vid == self) continue;
            const AliasSet *fa = bank.aliases_of(fi.fid);
            if (!fa || !ca->overlaps(*fa)) continue;
            if (vs <= fi.end && fi.start <= ve) return true; // rangos se solapan
        }
        return false;
    };

    // Activos: valor EN REGISTRO cuyo rango no ha terminado.
    struct Active { const AbstractValue *v; int lane; };
    std::vector<Active> active;

    auto lane_free = [&](uint8_t id) -> bool {
        const AliasSet *ca = bank.aliases_of(id);
        if (!ca) return false;
        for (const Active &a : active) {
            const AliasSet *aa = bank.aliases_of(static_cast<uint8_t>(a.lane));
            if (aa && ca->overlaps(*aa)) return false;
        }
        return true;
    };
    /* ¿Prohibe alguna restriccion de FORMA que @p value ocupe @p lane?  Mira si
     * la lane ya se la dieron a alguien con quien no puede compartirla. */
    auto prohibida_por_forma = [&](uint32_t value, int lane) -> bool {
        if (ctx.constraints == nullptr) return false;
        const ConstraintSet &cs = ctx.get_constraints();
        for (const Constraint &c : cs.items) {
            if (c.kind != ConstraintKind::DIFFERENT_LANE) continue;
            const uint32_t otro = (c.a == value) ? c.b
                                : (c.b == value) ? c.a
                                                 : UINT32_MAX;
            if (otro == UINT32_MAX) continue;
            if (out.lane_of(otro) == lane) return true;
        }
        return false;
    };

    auto first_free_lane = [&](const AbstractValue *v) -> int {
        const ValueRequirements &r = v->req;
        if (r.fixed_reg >= 0) {
            const uint8_t fid = static_cast<uint8_t>(r.fixed_reg);
            const Lane *l = bank.by_id(fid);
            // El pin NO exige @c is_allocatable: un @c register("rXX") es una
            // constraint de NIVEL SUPERIOR (ABI/asm que el usuario fijo, p.ej.
            // r10 = arg4 de un syscall).  Prevalece sobre la reserva de scratch
            // (r10/r11 los reserva el rewrite para DIVMOD/atomic/LOAD_VM, pero un
            // binding explicito los reclama).  Solo respeta las restricciones
            // duras: debe-memoria, lane prohibida (clobber que el valor atraviesa),
            // clase, ancho y que la lane este libre.
            if (!r.must_be_memory() && !r.lane_forbidden(fid) && l && l->cls == r.cls &&
                bank.supports(fid, r.width) && lane_free(fid))
                return fid;
            return kSpilled;
        }
        for (const Lane &l : bank.lanes) {
            if (!lane_admissible(r, l, vec_active)) continue; // correctitud dura (cero by_id).
            if (!lane_free(l.id)) continue;
            /* Lane que la FORMA de una instruccion prohibe compartir, aunque
             * los dos valores no coincidan en el tiempo.  Sin esto el
             * asignador los junta -- es legitimo por vidas -- y luego hay que
             * deshacerlo con dos movimientos y un temporal.  Barato de mirar:
             * el conjunto esta vacio salvo en las funciones que lo pidan. */
            if (prohibida_por_forma(v->value_id, l.id)) continue;
            // Un valor SIN pin no roba una lane RESERVADA por un pin que se solapa
            // (aunque ese pin aun no este activo): el pin la necesitara al llegar.
            if (lane_pinned_by_other(l.id, v->start, v->end, v->value_id)) continue;
            return l.id;
        }
        return kSpilled;
    };
    // "Conveniencia de derramar" c en el punto @p now: mas HORIZONTE hasta el
    // proximo uso y menos coste -> mejor victima.  Fase A (transicion a Belady):
    // se calculan AMBAS metricas -- remaining_life (duracion restante, heuristica
    // vieja) y next_use_distance (Belady) -- pero la DECISION usa el next-use si el
    // Fact esta cableado (@c ctx.has_next_use()); remaining_life queda de fallback y
    // para instrumentacion.  El coste sale del Objective; la fusion es del algoritmo.
    constexpr double kDeadHorizon = 1e18; // horizonte "infinito": victima muerta ideal.
    auto spill_score = [&](const AbstractValue *c, uint32_t now) -> double {
        /* Un valor que TIENE que estar en registro no es candidato a nada: su
         * uso lo nombra como registro y en memoria no existe.  Es el caso de
         * un operando de un bloque asm -- el cuerpo dice `movdqu v0, [s]`, y
         * `v0` no puede ser una posicion de pila.  Se le da la peor puntuacion
         * posible para que jamas salga elegido; si aun asi no cabe ninguno, el
         * problema es que se han pedido mas operandos de los que hay, y eso lo
         * cuenta quien reescribe. */
        if (c->req.residency == Residency::REGISTER) return -1.0;
        const double remaining =
            static_cast<double>(c->end >= now ? c->end - now : 0) + 1.0;
        const double cost = spill_cost_via_objective(c->req, ctx);
        double horizon = remaining; // fallback: duracion restante (pre-Belady).
        if (ctx.has_next_use()) {
            const uint32_t d =
                ctx.next_use_distance(c->value_id, codegen::LinearPos{now});
            // d == UINT32_MAX (muerto) -> horizonte infinito (victima ideal);
            // si no, distancia real al proximo uso (Belady).
            horizon = (d == UINT32_MAX) ? kDeadHorizon
                                        : static_cast<double>(d) + 1.0;
        }
        return horizon / (cost > 0.0 ? cost : 1e-9); // grande = mejor victima.
    };
    // Instrumento (opcional): clasifica POR QUE se eligio una victima.
    auto record_victim = [&](const AbstractValue *victim, uint32_t now) {
        if (!ctx.spill_trace) return;
        ++ctx.spill_trace->spills_total;
        if (!ctx.has_next_use()) return;
        const uint32_t d =
            ctx.next_use_distance(victim->value_id, codegen::LinearPos{now});
        if (d == UINT32_MAX) ++ctx.spill_trace->victims_dead;  // muerta (Belady trivial).
        else                 ++ctx.spill_trace->victims_alive; // uso lejano (Belady real).
    };

    for (const AbstractValue *v : order) {
        // Expirar activos muertos.
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&](const Active &a) { return a.v->end < v->start; }),
                     active.end());

        const int lane = first_free_lane(v);
        if (lane != kSpilled) {
            out.assign(v->value_id, lane);
            active.push_back({v, lane});
            continue;
        }

        // No cabe -> elegir victima entre v y los activos de la MISMA clase que
        // aliasarian (candidatos que liberarian una lane usable por v).  El pin de v
        // restringe: si v esta pinado, solo su lane sirve.
        const AbstractValue *victim = v;
        double best = spill_score(v, v->start);
        for (const Active &a : active) {
            // La lane que robariamos a la victima debe ser ADMISIBLE para v: misma
            // clase, soporta el ancho y -- si v cruza un CALL -- ser callee-saved.
            // Asi un cross-call nunca roba una lane volatil.  Si v esta PINADO, el pin
            // (restriccion de nivel superior: ABI/asm) manda: solo su lane sirve.
            if (v->req.fixed_reg >= 0) {
                if (a.lane != v->req.fixed_reg) continue;
            } else if (!lane_admissible(v->req, static_cast<uint8_t>(a.lane), bank,
                                        vec_active)) {
                continue;
            }
            const double sc = spill_score(a.v, v->start);
            if (sc > best) { best = sc; victim = a.v; }
        }

        record_victim(victim, v->start); // instrumento: razon de la victima.
        if (victim == v) {
            /* Derramar un valor que TIENE que estar en registro no es una
             * decision, es una imposibilidad: su uso lo nombra como registro.
             * Se cuenta en voz alta con el motivo por el que ninguna lane
             * valia, que es lo que hace falta para arreglarlo. */
            if (v->req.residency == Residency::REGISTER) {
                LaneHazard razon = LaneHazard::NONE;
                unsigned de_su_clase = 0, admisibles = 0, libres = 0;
                for (const Lane &l : bank.lanes) {
                    if (l.cls != v->req.cls) continue; // otra clase: no dice nada
                    ++de_su_clase;
                    const LaneHazard h = lane_hazard(v->req, l, vec_active);
                    if (h != LaneHazard::NONE) {
                        if (razon == LaneHazard::NONE) razon = h;
                        continue;
                    }
                    ++admisibles;
                    if (lane_free(l.id)) ++libres;
                }
                std::fprintf(stderr,
                             "[rbank] el valor %u tiene que estar en registro "
                             "y no lo consigue: %u lanes de su clase, %u "
                             "admisibles, %u libres, primer impedimento %d\n",
                             v->value_id, de_su_clase, admisibles, libres,
                             static_cast<int>(razon));
            }
            out.spill(v->value_id); // v es la peor de mantener -> derramar v.
        } else {
            // Derramar la victima, dar su lane a v.
            int freed_lane = kSpilled;
            for (size_t i = 0; i < active.size(); ++i)
                if (active[i].v == victim) { freed_lane = active[i].lane;
                    active.erase(active.begin() + i); break; }
            out.spill(victim->value_id);
            out.assign(v->value_id, freed_lane);
            active.push_back({v, freed_lane});
        }
    }
    return out;
}

/**
 * @brief Coste TOTAL de spill de una asignacion (suma del coste -- del Objective --
 *        de los valores derramados).  Metrica para comparar estrategias de spill.
 */
inline double total_spill_cost(const AbstractProblem &p, const LaneAssignment &a,
                               const OptimizationContext &ctx) {
    double sum = 0.0;
    for (const AbstractValue &v : p.values)
        if (a.lane_of(v.value_id) == kSpilled)
            sum += spill_cost_via_objective(v.req, ctx);
    return sum;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_SMART_SPILL_H
