/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/backend_bridge.h
 * @brief BACKEND BRIDGE (F5): traduccion MECANICA entre el resultado del modelo
 *        (@c LaneAssignment) y el que consume el backend (@c jit::RegAlloc), en
 *        ambos sentidos.  Es un puente DELIBERADAMENTE TONTO: cuanto menos decide,
 *        menos superficie de bugs introduce al meter rbank en produccion.
 *
 *     LaneAssignment  --regalloc_from_lanes-->  jit::RegAlloc  --> rewrite_to_physical
 *     jit::RegAlloc   --regalloc_to_lanes---->  LaneAssignment --> is_proper_coloring
 *
 * REGLAS del puente (no decide, solo mapea):
 *   - lane fisica -> @c reg;  @c kSpilled -> spill slot (uno por valor derramado);
 *   - conserva el resto del contrato que @c rewrite_to_physical espera:
 *     @c callee_saved_used (los PRESERVED usados, para el prologue/epilogue) y
 *     @c num_spill_slots.
 *
 * CORRECTITUD del envolvente (por que es seguro): el modelo usa el ENVOLVENTE del
 * LiveRange, que AGRANDA los rangos.  Por tanto SOBRE-estima la interferencia (dos
 * valores que solapan de verdad tienen envolventes que tambien solapan) y NUNCA la
 * subestima -> el modelo jamas comparte un registro entre valores que realmente
 * coinciden.  Puede derramar de mas (suboptimo), nunca incorrecto.  El oraculo final
 * sigue siendo el backend (rewrite -> encode -> diff_harness -> e2e).
 *
 * i18n: produce DATOS.  ADITIVO.
 */

#ifndef VESTA_CODEGEN_RBANK_BACKEND_BRIDGE_H
#define VESTA_CODEGEN_RBANK_BACKEND_BRIDGE_H

#include "codegen/rbank/abstract_problem.h"
#include "codegen/rbank/constraints.h"
#include "codegen/rbank/physical_bank.h"
#include "jit/linear_scan.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @brief @c jit::RegAlloc -> @c LaneAssignment (para validar la asignacion de
 *        PRODUCCION con @c is_proper_coloring / @c validate_coloring).  REG -> lane
 *        (id fisico); todo lo demas (SPILL/NONE/ausente) -> @c kSpilled.
 */
inline LaneAssignment regalloc_to_lanes(const jit::RegAlloc &ra,
                                        const AbstractProblem &p) {
    LaneAssignment la;
    for (const AbstractValue &v : p.values) {
        if (v.value_id < ra.assign.size() &&
            ra.assign[v.value_id].loc == jit::RegAlloc::Loc::REG)
            la.assign(v.value_id, ra.assign[v.value_id].reg);
        else
            la.spill(v.value_id);
    }
    return la;
}

/**
 * @brief @c LaneAssignment -> @c jit::RegAlloc (para meter la asignacion del modelo
 *        en el backend de PRODUCCION).  Puente tonto:
 *          - lane -> @c reg (loc=REG);  @c kSpilled -> slot nuevo (loc=SPILL);
 *          - @c assign es DENSO 0..vreg_count-1: los vregs ausentes/muertos quedan
 *            @c Loc::NONE (el backend los ignora);
 *          - @c callee_saved_used = los PRESERVED usados, deduplicados y ordenados
 *            (el prologue/epilogue push/pop justo estos);
 *          - @c num_spill_slots = un slot por valor derramado (sin reuso: mas pila
 *            pero trivialmente correcto -- la eficiencia de slots es otra fase).
 *
 * @param vreg_count  tamano DENSO del vector assign (max vreg id + 1).  Lo conoce el
 *        llamante (p.ej. @c ivs.intervals.size()); los valores del problema son un
 *        subconjunto (los intervalos no vacios).
 */
inline jit::RegAlloc regalloc_from_lanes(const LaneAssignment &la,
                                         const AbstractProblem &p,
                                         const PhysicalRegisterBank &bank,
                                         uint32_t vreg_count) {
    jit::RegAlloc ra;
    ra.assign.assign(vreg_count, jit::RegAlloc::VAssign{}); // todos NONE.
    uint32_t next_slot = 0;
    std::vector<uint8_t> callee; // PRESERVED usados (dedup, orden de insercion).

    for (const AbstractValue &v : p.values) {
        if (v.value_id >= vreg_count) continue; // defensivo: fuera del rango denso.
        const int lane = la.lane_of(v.value_id);
        jit::RegAlloc::VAssign a;
        if (lane == kSpilled) {
            a.loc = jit::RegAlloc::Loc::SPILL;
            a.slot = next_slot++;
        } else {
            a.loc = jit::RegAlloc::Loc::REG;
            a.reg = static_cast<uint8_t>(lane);
            if (bank.preservation(a.reg, v.req.width) == SavePolicy::PRESERVED &&
                std::find(callee.begin(), callee.end(), a.reg) == callee.end())
                callee.push_back(a.reg);
        }
        ra.assign[v.value_id] = a;
    }

    std::sort(callee.begin(), callee.end()); // orden estable/determinista.
    ra.callee_saved_used = std::move(callee);
    ra.num_spill_slots = next_slot;
    return ra;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_BACKEND_BRIDGE_H
