/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/frame_layout.h
 * @brief @c FrameLayout: describe el ESTADO FISICO del stack que implica una
 * asignacion. Responde UNA pregunta -- ¿como queda el frame? (que callee-saved
 * preservar, cuantos spill slots).  NO depende del tiempo: NO es un timeline.
 *
 * Separado de @c AllocationTimeline a proposito: "donde vive el valor en cada
 * instante" y "como es el frame" son preguntas distintas.  La separacion
 * permite, ademas, generar el MISMO timeline con frames distintos (SysV vs
 * Win64, compactado vs alineado) sin tocar la ubicacion temporal.  El @c
 * TimelineBuilder deriva ambos de la @c RegAlloc.
 */

#ifndef VESTA_CODEGEN_FRAME_LAYOUT_H
#define VESTA_CODEGEN_FRAME_LAYOUT_H

#include <cstdint>
#include <vector>

namespace codegen {

/**
 * @struct FrameLayout
 * @brief Estado fisico del frame implicado por una asignacion (no temporal).
 */
struct FrameLayout {
    /// Registros callee-saved usados (deduplicados, orden estable).  El
    /// prologue/epilogue debe push/pop estos.
    std::vector<uint8_t> callee_saved_used;
    /// Numero de spill slots reservados (cada uno @c pointer_size bytes).
    uint32_t num_spill_slots = 0;
};

} // namespace codegen

#endif // VESTA_CODEGEN_FRAME_LAYOUT_H
