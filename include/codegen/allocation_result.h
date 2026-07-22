/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/allocation_result.h
 * @brief @c AllocationResult: lo que el Rewrite consume.  NO es un God Object -- son las
 *        DOS cosas que el Rewrite necesita, cada una respondiendo su propia pregunta y
 *        sin invadir la del otro:
 *
 *     AllocationResult
 *       ├── FrameLayout        ¿como queda el frame?          (no temporal)
 *       └── AllocationTimeline ¿donde vive cada valor cuando? (temporal, puro)
 *
 * El Rewrite conoce SOLO dos conceptos: @c result.frame y @c result.timeline.lookup(
 * vreg, pos).  Nada mas.  Lugar del pipeline:
 *
 *     LaneAssignment -> FragmentationRecovery -> SplitPlan -> TimelineBuilder
 *                                                                 |
 *                                                          AllocationResult
 *                                                          (frame + timeline)
 *                                                                 |
 *                                                              Rewrite
 */

#ifndef VESTA_CODEGEN_ALLOCATION_RESULT_H
#define VESTA_CODEGEN_ALLOCATION_RESULT_H

#include "codegen/allocation_timeline.h"
#include "codegen/frame_layout.h"

namespace codegen {

/**
 * @struct AllocationResult
 * @brief Frame + timeline: la asignacion completa que consume el Rewrite.
 */
struct AllocationResult {
    FrameLayout        frame;    ///< estado fisico del stack (no temporal).
    AllocationTimeline timeline; ///< ubicacion de cada valor en el tiempo.
};

} // namespace codegen

#endif // VESTA_CODEGEN_ALLOCATION_RESULT_H
