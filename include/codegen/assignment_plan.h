/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/assignment_plan.h
 * @brief @c AssignmentPlan: el PLAN de una transformacion de asignacion.
 * Responde UNA pregunta -- ¿donde debe vivir cada valor durante cada tramo?
 *
 * Un @c AssignmentInterval es una AFIRMACION INDEPENDIENTE:
 *
 *     entre [from, to), el valor @c vreg vive en @c location.
 *
 * Punto.  NO se define "respecto a una asignacion base": manana puede no haber
 * base (varios transformadores encadenados).  El @c TimelineBuilder decide como
 * combinar estas afirmaciones con el resto -- el intervalo, por si solo, ya
 * dice todo lo que afirma.
 *
 * POR QUE EXISTE: desacopla la DECISION ("este tramo debe vivir aqui") de la
 * REPRESENTACION FINAL ("como queda distribuido el valor en el tiempo").  La
 * decision la toma una transformacion; la representacion la materializa el @c
 * TimelineBuilder:
 *
 *     Conocimiento  ->  AssignmentPlan  ->  AllocationTimeline  ->  Rewrite
 *       (Facts)          (decision)         (materializacion)      (codigo)
 *
 * NO pertenece al splitting -- el splitting es solo UNO de sus productores.  La
 * Fragmentation Recovery produce varios intervalos por valor; la Recovery
 * produce uno que cubre toda la vida; manana el Coalescing o la
 * rematerializacion parcial produciran los suyos.  Todos hablan el MISMO
 * lenguaje y el materializador no sabe cual lo creo.  Por eso el tipo no se
 * llama "Split": el concepto es INDEPENDIENTE del algoritmo que lo genera.
 *
 * PROVISIONAL (incremento actual): el productor del plan ya proporciona la @c
 * location materializada (que registro, que slot).  En el diseno completo esa
 * informacion podra venir de una FASE POSTERIOR de asignacion de lanes -- el
 * productor diria solo "este tramo merece registro" y otra fase elegiria CUAL
 * -- sin modificar el @c TimelineBuilder.  Se acepta hoy para mantener el
 * incremento pequeno; NO es un contrato arquitectonico definitivo.
 *
 * Posiciones en el dominio MachineIR (@c codegen::LinearPos), semiabiertas
 * [from, to) como los @c LiveRange.  Plan vacio = sin afirmaciones (la
 * asignacion pasa tal cual al Timeline)
 * -> el pipeline SIEMPRE existe, sin casos especiales.
 */

#ifndef VESTA_CODEGEN_ASSIGNMENT_PLAN_H
#define VESTA_CODEGEN_ASSIGNMENT_PLAN_H

#include "codegen/linear_pos.h"
#include "codegen/value_location.h"

#include <cstdint>
#include <vector>

namespace codegen {

/**
 * @struct AssignmentInterval
 * @brief Una AFIRMACION: entre @c [from, to), el valor @c vreg vive en @c
 * location.  Un TRAMO (no un punto), independiente de quien lo produjo y de si
 * existe o no una asignacion previa.
 */
struct AssignmentInterval {
    uint32_t vreg = 0;      ///< valor afectado (denso, dominio MachineIR).
    LinearPos from;         ///< inicio del tramo (inclusive).
    LinearPos to;           ///< fin del tramo (exclusive).
    ValueLocation location; ///< donde vive el valor DURANTE el tramo.
};

/**
 * @struct AssignmentPlan
 * @brief Coleccion de afirmaciones que produce una transformacion de
 * asignacion. Vacio = sin afirmaciones.
 */
struct AssignmentPlan {
    std::vector<AssignmentInterval> intervals;

    bool empty() const noexcept { return intervals.empty(); }
    /// Afirma que @p vreg vive en @p location durante @c [from, to).
    void add(uint32_t vreg, LinearPos from, LinearPos to,
             ValueLocation location) {
        intervals.push_back({vreg, from, to, location});
    }
};

} // namespace codegen

#endif // VESTA_CODEGEN_ASSIGNMENT_PLAN_H
