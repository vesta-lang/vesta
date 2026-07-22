/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/split_plan.h
 * @brief @c SplitPlan: el PLAN de una transformacion de asignacion.  Responde UNA
 *        pregunta -- ¿que parte de que valor debe modificarse respecto a la asignacion
 *        base?
 *
 * POR QUE EXISTE: desacopla la DECISION ("este tramo merece otra ubicacion") de la
 * REPRESENTACION FINAL ("como queda distribuido el valor en el tiempo").  La decision la
 * toma una transformacion; la representacion la materializa el @c TimelineBuilder.  Sin
 * este intermedio, la transformacion tendria que escribir directamente el modelo temporal
 * -- mezclar decision y materializacion, justo lo que la filosofia del backend evita:
 *
 *     Conocimiento  ->  SplitPlan  ->  AllocationTimeline  ->  Rewrite
 *       (Facts)         (decision)      (materializacion)      (codigo)
 *
 * En la jerarquia del backend:
 *
 *     SSA IR -> MachineIR -> SplitPlan (decision) -> AllocationTimeline (materializa) -> Rewrite
 *
 * Lo produce CUALQUIER transformacion de asignacion -- NO pertenece al splitting, sino al
 * backend: Fragmentation Recovery / Splitting; tambien la Recovery (un unico tramo que
 * cubre toda la vida); manana rematerializacion parcial u otras.  Todas producen el mismo
 * plan; el @c TimelineBuilder no sabe cual lo creo.
 *
 * Posiciones en el dominio MachineIR (@c codegen::LinearPos), semiabiertas [from, to) como
 * los @c LiveRange de @c build_intervals.  Vacio = sin cambios (la asignacion base pasa tal
 * cual al Timeline) -> el pipeline SIEMPRE existe, sin casos especiales.
 */

#ifndef VESTA_CODEGEN_SPLIT_PLAN_H
#define VESTA_CODEGEN_SPLIT_PLAN_H

#include "codegen/linear_pos.h"

#include <cstdint>
#include <vector>

namespace codegen {

/**
 * @struct SplitInterval
 * @brief Un TRAMO (no un punto): describe una modificacion TEMPORAL respecto a la
 *        asignacion base para el vreg @c vreg durante @c [from, to).  El tramo modifica
 *        temporalmente la base; el @c TimelineBuilder resuelve la distribucion completa.
 *        (Hoy la modificacion es "promover a registro"; el concepto admite otras -- MEM,
 *        rematerializacion, registro vectorial... -- sin cambiar de forma.)
 */
struct SplitInterval {
    uint32_t  vreg = 0;   ///< valor afectado (denso, dominio MachineIR).
    LinearPos from;       ///< inicio del tramo (inclusive).
    LinearPos to;         ///< fin del tramo (exclusive).
};

/**
 * @struct SplitPlan
 * @brief Coleccion de @c SplitInterval que produce una transformacion de asignacion.
 *        Vacio = sin cambios (la asignacion base pasa tal cual al Timeline).
 */
struct SplitPlan {
    std::vector<SplitInterval> intervals;

    bool empty() const noexcept { return intervals.empty(); }
    void add(uint32_t vreg, LinearPos from, LinearPos to) {
        intervals.push_back({vreg, from, to});
    }
};

} // namespace codegen

#endif // VESTA_CODEGEN_SPLIT_PLAN_H
