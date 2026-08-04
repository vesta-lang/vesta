/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/backend_bridge.h
 * @brief BACKEND BRIDGE (F5): traduccion MECANICA entre el resultado del modelo
 *        (@c LaneAssignment) y el que consume el backend (@c codegen::RegAlloc), en
 *        ambos sentidos.  Es un puente DELIBERADAMENTE TONTO: cuanto menos decide,
 *        menos superficie de bugs introduce al meter rbank en produccion.
 *
 *     LaneAssignment  --regalloc_from_lanes-->  codegen::RegAlloc  --> rewrite_to_physical
 *     codegen::RegAlloc   --regalloc_to_lanes---->  LaneAssignment --> is_proper_coloring
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
#include "codegen/rbank/value_requirements.h"
#include "jit/interval.h"
#include "codegen/regalloc.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @brief Mapea la clase del backend (jit::RegClass) a la del modelo.
 *
 * HERENCIA HISTORICA (a vigilar): @c RegClass::FP -> @c FP_VECTOR porque el backend
 * SSE usa XMM para TODO (escalar y vectorial), asi que hoy coinciden FISICAMENTE.  El
 * dia que el banco separe FP-escalar de FP-vectorial, esto devolvera la clase escalar.
 */
inline ResourceClass resource_class_from_reg(jit::RegClass c) {
    return c == jit::RegClass::FP ? ResourceClass::FP_VECTOR : ResourceClass::GP;
}

/**
 * @brief ADAPTADOR BACKEND -> MODELO: extrae un @c AbstractProblem de los intervalos
 *        reales.  Solo EXTRAE Facts (traduce los hazards que el backend conoce a
 *        restricciones del valor); no decide nada.  Es la PUERTA DE ENTRADA unica de
 *        restricciones del backend al modelo -- parte del bridge de PRODUCCION.
 *
 * Por cada @c LiveInterval no vacio: value_id=vreg, [start,end]=envolvente, clase, pin,
 * y los HAZARDS traducidos:
 *   - cross-call (call_positions),
 *   - debe-memoria (residency=MEMORY): GC root cross-call + force_spill (live-in a un
 *     handler abnormal); UN solo mecanismo, varios origenes,
 *   - lanes prohibidas (forbidden_lanes): asm_clobbers que el valor atraviesa.
 */
inline AbstractProblem intervals_to_problem(const jit::IntervalResult &ivs) {
    AbstractProblem p;
    p.values.reserve(ivs.intervals.size());
    for (const jit::LiveInterval &iv : ivs.intervals) {
        if (iv.ranges.empty()) continue; // vreg muerto: el allocator lo ignora.
        AbstractValue av;
        av.value_id = iv.vreg;
        // TODO(RangeSet): el modelo PIERDE LOS HUECOS aqui -- toma el envolvente
        // [first.from, last.to) porque AbstractValue es de un solo segmento.  Casi
        // todos los problemas futuros CONVERGEN aqui (coalescing, next-use/Belady,
        // presion exacta); desaparecen cuando AbstractValue sea MULTI-SEGMENTO.
        av.start = iv.ranges.front().from;
        av.end = iv.ranges.back().to > 0 ? iv.ranges.back().to - 1 : 0; // ) -> ]
        av.req.value_id = iv.vreg;
        av.req.cls = resource_class_from_reg(iv.cls);
        av.req.width = iv.cls == jit::RegClass::FP ? ViewWidth::W16 : ViewWidth::W8;
        av.req.fixed_reg = static_cast<int16_t>(iv.fixed_reg); // -1 o el pin.
        av.req.is_gc = iv.gc_kind != 0;
        for (uint32_t cp : ivs.call_positions) {
            for (const jit::LiveRange &r : iv.ranges)
                if (cp >= r.from && cp < r.to) { av.req.crosses_call = true; break; }
            if (av.req.crosses_call) break;
        }
        // HAZARD "debe-memoria" (residency=MEMORY): UN mecanismo, dos origenes -- GC
        // root cross-call (stackmap) + force_spill (live-in a handler abnormal; el
        // throw clobbea TODOS los regs, solo la memoria sobrevive).
        if ((av.req.crosses_call && av.req.is_gc) ||
            (iv.vreg < ivs.force_spill.size() && ivs.force_spill[iv.vreg]))
            av.req.residency = Residency::MEMORY;
        // HAZARD "lanes muertas" (forbidden_lanes): las lanes que un INLINE_ASM destruye
        // en un punto que el valor atraviesa (incluye callee-saved que el CALL no protege).
        for (const jit::IntervalResult::AsmClobberSite &site : ivs.asm_clobbers) {
            bool covers = false;
            for (const jit::LiveRange &r : iv.ranges)
                if (site.pos >= r.from && site.pos < r.to) { covers = true; break; }
            if (!covers) continue;
            for (uint8_t cr : site.regs)
                if (cr < 64) av.req.forbidden_lanes |= (1ull << cr);
        }
        p.values.push_back(av);
    }
    return p;
}

/**
 * @brief @c codegen::RegAlloc -> @c LaneAssignment (para validar la asignacion de
 *        PRODUCCION con @c is_proper_coloring / @c validate_coloring).  REG -> lane
 *        (id fisico); todo lo demas (SPILL/NONE/ausente) -> @c kSpilled.
 */
inline LaneAssignment regalloc_to_lanes(const codegen::RegAlloc &ra,
                                        const AbstractProblem &p) {
    LaneAssignment la;
    for (const AbstractValue &v : p.values) {
        if (v.value_id < ra.assign.size() &&
            ra.assign[v.value_id].loc == codegen::RegAlloc::Loc::REG)
            la.assign(v.value_id, ra.assign[v.value_id].reg);
        else
            la.spill(v.value_id);
    }
    return la;
}

/**
 * @brief @c LaneAssignment -> @c codegen::RegAlloc (para meter la asignacion del modelo
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
inline codegen::RegAlloc regalloc_from_lanes(const LaneAssignment &la,
                                         const AbstractProblem &p,
                                         const PhysicalRegisterBank &bank,
                                         uint32_t vreg_count) {
    codegen::RegAlloc ra;
    ra.assign.assign(vreg_count, codegen::RegAlloc::VAssign{}); // todos NONE.
    uint32_t next_slot = 0;
    std::vector<uint8_t> callee; // PRESERVED usados (dedup, orden de insercion).

    for (const AbstractValue &v : p.values) {
        if (v.value_id >= vreg_count) continue; // defensivo: fuera del rango denso.
        const int lane = la.lane_of(v.value_id);
        codegen::RegAlloc::VAssign a;
        if (lane == kSpilled) {
            a.loc = codegen::RegAlloc::Loc::SPILL;
            a.slot = next_slot++;
        } else {
            a.loc = codegen::RegAlloc::Loc::REG;
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
