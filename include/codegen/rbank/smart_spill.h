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
    auto first_free_lane = [&](const ValueRequirements &r) -> int {
        if (r.fixed_reg >= 0) {
            const uint8_t fid = static_cast<uint8_t>(r.fixed_reg);
            const Lane *l = bank.by_id(fid);
            if (l && l->cls == r.cls && bank.is_allocatable(fid, vec_active) &&
                bank.supports(fid, r.width) && lane_free(fid))
                return fid;
            return kSpilled;
        }
        for (const Lane &l : bank.lanes) {
            if (!lane_admissible(r, l, vec_active)) continue; // correctitud dura (cero by_id).
            if (!lane_free(l.id)) continue;
            return l.id;
        }
        return kSpilled;
    };
    // "Conveniencia de derramar" c en el punto @p now: mas tiempo liberado y menos
    // coste -> mejor victima.  El coste sale del Objective; el tiempo, del LiveRange.
    auto spill_score = [&](const AbstractValue *c, uint32_t now) -> double {
        const double freed = static_cast<double>(c->end >= now ? c->end - now : 0) + 1.0;
        const double cost = spill_cost_via_objective(c->req, ctx);
        return freed / (cost > 0.0 ? cost : 1e-9); // grande = mejor victima.
    };

    for (const AbstractValue *v : order) {
        // Expirar activos muertos.
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&](const Active &a) { return a.v->end < v->start; }),
                     active.end());

        const int lane = first_free_lane(v->req);
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

        if (victim == v) {
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
