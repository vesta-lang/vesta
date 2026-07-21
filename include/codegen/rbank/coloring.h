/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/coloring.h
 * @brief Fase 0.5: COLOREADO por lane del problema abstracto + verificacion de
 *        round-trip.  Cierra el ciclo: LiveRanges -> Interference -> Coloring.
 *
 *     AbstractProblem  --build_interference-->  ConstraintSet
 *            |                                        |
 *     color_linear_scan  (asigna value -> LANE)       |
 *            |                                        |
 *       LaneAssignment  --is_proper_coloring----------+  (validate_assignment)
 *
 * ALGORITMO (left-edge / linear-scan): barrer los valores por @c start, mantener
 * los "activos" (aun vivos), y asignar a cada valor la PRIMERA lane de su clase
 * libre (no ocupada por un activo que aliasa) que satisfaga sus requisitos.  Para
 * un INTERVAL GRAPH esto es OPTIMO: usa exactamente @c max_overlap colores por
 * clase (P2).  Consecuencia (la PROPIEDAD que valida el modelo): si el banco
 * ofrece >= max_overlap(clase) lanes asignables de esa clase, y ningun requisito
 * lo impide, el coloreador NO derrama ni un valor de esa clase.
 *
 * El algoritmo piensa en LANE, nunca en xmm/ymm/zmm (P3): consulta el banco.  La
 * verificacion NO reimplementa la interferencia: reusa @c validate_assignment (la
 * espina dorsal) -> lo que valida el round-trip es lo mismo que validaria el
 * allocator real.
 *
 * i18n: produce DATOS (asignacion/numeros).  Fase 0.5: ADITIVO, sin consumidores
 * de produccion (solo el prototipo/test).
 */

#ifndef VESTA_CODEGEN_RBANK_COLORING_H
#define VESTA_CODEGEN_RBANK_COLORING_H

#include "codegen/rbank/abstract_problem.h"
#include "codegen/rbank/allowed_lanes.h"
#include "codegen/rbank/constraints.h"
#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/value_requirements.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @brief Colorea el problema abstracto por LANE (left-edge / linear-scan).
 * @param vec_active  si la funcion usa el path de reduccion vectorial (afecta a
 *                    las lanes VEC_ACC demand-driven que cuentan como asignables).
 * @return asignacion value_id -> lane (o @c kSpilled si no cupo).
 *
 * Determinista: barre por (start, value_id) y prueba las lanes en el orden del
 * banco -> mismo problema, mismo coloreado.
 */
inline LaneAssignment color_linear_scan(const AbstractProblem &p,
                                        const PhysicalRegisterBank &bank,
                                        bool vec_active) {
    LaneAssignment out;

    // Orden de barrido: por punto de definicion; desempate por value_id (estable).
    std::vector<const AbstractValue *> order;
    order.reserve(p.values.size());
    for (const AbstractValue &v : p.values) order.push_back(&v);
    std::sort(order.begin(), order.end(),
              [](const AbstractValue *a, const AbstractValue *b) {
                  if (a->start != b->start) return a->start < b->start;
                  return a->value_id < b->value_id;
              });

    // Activos: valores EN REGISTRO cuyo rango aun no ha terminado.
    struct Active { uint32_t end; int lane; };
    std::vector<Active> active;

    // ¿La lane @p id esta libre respecto a los activos?  Ocupada si su AliasSet
    // solapa con el de algun activo -> captura el aliasing sub-registro (P3).
    auto lane_free = [&](uint8_t id) -> bool {
        const AliasSet *ca = bank.aliases_of(id);
        if (!ca) return false;
        for (const Active &a : active) {
            const AliasSet *aa = bank.aliases_of(static_cast<uint8_t>(a.lane));
            if (aa && ca->overlaps(*aa)) return false;
        }
        return true;
    };

    for (const AbstractValue *v : order) {
        // Expirar los activos que ya murieron antes de que este valor nazca.
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&](const Active &a) { return a.end < v->start; }),
                     active.end());

        int chosen = kSpilled;
        const ValueRequirements &r = v->req;

        if (r.fixed_reg >= 0) {
            // Pin duro: solo esa lane (si es de la clase, asignable, soporta el
            // ancho y esta libre).  Si no, el valor se derrama (infactible).
            const uint8_t fid = static_cast<uint8_t>(r.fixed_reg);
            const Lane *l = bank.by_id(fid);
            if (l && l->cls == r.cls && bank.is_allocatable(fid, vec_active) &&
                bank.supports(fid, r.width) && lane_free(fid))
                chosen = fid;
        } else {
            // Primera lane ADMISIBLE (correctitud dura, via AllowedLaneSet) y libre.
            // El coloreo no interpreta crosses_call: solo pregunta lane_admissible.
            for (const Lane &l : bank.lanes) {
                if (!lane_admissible(r, l, vec_active)) continue; // version pura (cero by_id).
                if (!lane_free(l.id)) continue;
                chosen = l.id;
                break;
            }
        }

        if (chosen == kSpilled) {
            out.spill(v->value_id);
        } else {
            out.assign(v->value_id, chosen);
            active.push_back({v->end, chosen});
        }
    }
    return out;
}

/**
 * @brief ROUND-TRIP: ¿es @p a un coloreado PROPIO del problema @p p?  Reusa
 *        @c validate_assignment (la espina dorsal) sobre la interferencia
 *        RE-ABSTRAIDA de los LiveRanges -> lo que se valida es identico a lo que
 *        validaria el allocator real.
 */
/**
 * @struct ColoringValidation
 * @brief Veredicto de correctitud DESGLOSADO por categoria (no un bool).  Cuando el
 *        shadow falla sobre un corpus grande, esto dice QUE parte del modelo esta
 *        incompleta (reserved=1 vs crosscall=3 vs overlap=0...) en vez de solo
 *        "invalid".  Convierte el shadow en un oraculo de regresiones.  DATOS puros.
 */
struct ColoringValidation {
    uint32_t overlap_errors   = 0; ///< dos valores que interfieren en lanes que aliasan.
    uint32_t crosscall_errors = 0; ///< valor cross-call en lane VOLATILE (caller-saved).
    uint32_t width_errors     = 0; ///< lane no soporta el ancho del valor.
    uint32_t class_errors     = 0; ///< lane de clase de recurso incorrecta.
    uint32_t reserved_errors  = 0; ///< lane no asignable (scratch/reservada/inexistente).
    uint32_t fixed_errors     = 0; ///< valor pinado no esta en su fixed_reg.

    uint32_t total() const noexcept {
        return overlap_errors + crosscall_errors + width_errors + class_errors +
               reserved_errors + fixed_errors;
    }
    bool ok() const noexcept { return total() == 0; }
    /** @brief Acumula (para el agregado del corpus). */
    void add(const ColoringValidation &o) noexcept {
        overlap_errors += o.overlap_errors;   crosscall_errors += o.crosscall_errors;
        width_errors += o.width_errors;       class_errors += o.class_errors;
        reserved_errors += o.reserved_errors; fixed_errors += o.fixed_errors;
    }
};

/**
 * @brief Valida un coloreo DESGLOSANDO cada tipo de violacion (no corta al primer
 *        error como @c validate_assignment).  Recorre requisitos por-valor (clase,
 *        ancho, asignabilidad, pin, cross-call) + interferencia (aristas cuyas lanes
 *        aliasan).  Un valor PINADO: el ABI manda -> solo se comprueba que este en su
 *        lane; las demas reglas no se le aplican (el pin es de nivel superior).
 */
inline ColoringValidation validate_coloring(const AbstractProblem &p,
                                            const LaneAssignment &a,
                                            const PhysicalRegisterBank &bank,
                                            bool vec_active) {
    ColoringValidation v;
    for (const AbstractValue &val : p.values) {
        const int lane = a.lane_of(val.value_id);
        if (lane == kSpilled) continue; // en memoria: no ocupa lane.
        const Lane *l = bank.by_id(static_cast<uint8_t>(lane));
        if (!l) { ++v.reserved_errors; continue; }
        if (val.req.fixed_reg >= 0) {
            if (lane != val.req.fixed_reg) ++v.fixed_errors; // pin: solo su lane.
            continue;
        }
        if (l->cls != val.req.cls) ++v.class_errors;
        if (!l->allocatable(vec_active)) ++v.reserved_errors;
        if (!l->supports(val.req.width)) ++v.width_errors;
        if (val.req.crosses_call &&
            l->preservation_of(val.req.width) != SavePolicy::PRESERVED)
            ++v.crosscall_errors;
    }
    const ConstraintSet inter = build_interference(p); // re-abstraccion de LiveRanges.
    for (const Constraint &c : inter.items) {
        if (c.kind != ConstraintKind::INTERFERE) continue;
        const int la = a.lane_of(c.a), lb = a.lane_of(c.b);
        if (la == kSpilled || lb == kSpilled) continue;
        const AliasSet *aa = bank.aliases_of(static_cast<uint8_t>(la));
        const AliasSet *ab = bank.aliases_of(static_cast<uint8_t>(lb));
        if (aa && ab && aa->overlaps(*ab)) ++v.overlap_errors;
    }
    return v;
}

inline bool is_proper_coloring(const AbstractProblem &p, const LaneAssignment &a,
                               const PhysicalRegisterBank &bank, bool vec_active) {
    return validate_coloring(p, a, bank, vec_active).ok();
}

/** @brief Numero de valores DERRAMADOS en la asignacion. */
inline uint32_t spill_count(const AbstractProblem &p, const LaneAssignment &a) {
    uint32_t n = 0;
    for (const AbstractValue &v : p.values)
        if (a.lane_of(v.value_id) == kSpilled) ++n;
    return n;
}

/** @brief Numero de valores DERRAMADOS de la clase @p cls. */
inline uint32_t spill_count(const AbstractProblem &p, const LaneAssignment &a,
                            ResourceClass cls) {
    uint32_t n = 0;
    for (const AbstractValue &v : p.values)
        if (v.req.cls == cls && a.lane_of(v.value_id) == kSpilled) ++n;
    return n;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_COLORING_H
