/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/allocate.h
 * @brief ENTRADA de alto nivel del allocator rbank: intervalos del backend -> banco
 *        del path -> coloreo -> jit::RegAlloc.  Es el allocator de PRODUCCION UNICO
 *        (linear_scan jubilado); el orquestador (vreg_pipeline) solo LLAMA aqui, no
 *        contiene el allocator.
 *
 *     IntervalResult + TargetRegInfo         (del backend, por-path)
 *              |
 *     intervals_to_problem  (backend_bridge: traduce hazards -> Facts del valor)
 *              |
 *     color_smart_spill     (el modelo decide: coloreo por lane, spill por coste)
 *              |
 *     regalloc_from_lanes   (backend_bridge: LaneAssignment -> jit::RegAlloc)
 *
 * El banco se construye DESDE @p tri (el mismo del path: VM_ABI del JIT o
 * @c target.reg_info() del AOT) -> el modelo describe EXACTAMENTE el problema que ve
 * el backend/rewrite de ese path.  rbank es la fuente de conocimiento CENTRALIZADA:
 * si un caso no se puede representar, la politica es ABORTAR y anyadir el soporte al
 * MODELO (no un fallback legacy).
 *
 * DESACOPLADO del IR: recibe @p vec_active como bool (lo calcula el caller: detectar
 * ops VEC_* es del frontend/pipeline, no del allocator) -> este modulo solo depende de
 * jit (IntervalResult/RegAlloc/TargetRegInfo/BackendCaps) + el modelo rbank.
 */

#ifndef VESTA_CODEGEN_RBANK_ALLOCATE_H
#define VESTA_CODEGEN_RBANK_ALLOCATE_H

#include "codegen/rbank/backend_bridge.h"      // intervals_to_problem, regalloc_from_lanes
#include "codegen/rbank/optimization_context.h"
#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/smart_spill.h"         // color_smart_spill
#include "jit/backend_caps.h"
#include "jit/interval.h"
#include "jit/linear_scan.h"
#include "jit/target_reginfo.h"

#include <cstdint>

namespace codegen {
namespace rbank {

/**
 * @brief Asigna registros con rbank.  @p vreg_count = tamano DENSO (mf.vreg_count);
 *        @p tri = descriptor del path (JIT/AOT); @p vec_active = usa el path vectorial
 *        (afecta a las lanes VEC_ACC demand-driven).  El ssa_coalesce del backend ya
 *        elimino las copias -> el modelo NO re-coalesce (cada vreg tiene su asignacion).
 */
inline jit::RegAlloc rbank_allocate(const jit::IntervalResult &ivs, uint32_t vreg_count,
                                    const jit::TargetRegInfo &tri, bool vec_active) {
    PhysicalRegisterBank bank =
        physical_bank_x86_64_from_reginfo(tri, jit::backend_caps_host());
    ConstraintSet cs;
    OptimizationContext ctx = make_context(bank, cs);
    const AbstractProblem p = intervals_to_problem(ivs);
    const LaneAssignment la = color_smart_spill(p, ctx, vec_active);
    return regalloc_from_lanes(la, p, bank, vreg_count);
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_ALLOCATE_H
