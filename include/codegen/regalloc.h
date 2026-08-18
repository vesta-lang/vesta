/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/regalloc.h
 * @brief RESULTADO de la asignacion de registros -- estructura GENERAL del
 * codegen, NO del JIT.  La consumen los tres consumidores del backend
 * (interp/JIT via rewrite, AOT via sus targets) y la produce el allocator
 * central @c rbank (regalloc_from_lanes) ademas del @c linear_scan legacy.  Por
 * eso vive en
 *        @c codegen:: y no en @c jit::.
 *
 * POD puro: por cada vreg denso una ubicacion (REG fisico / SPILL slot / NONE
 * muerto), mas los callee-saved usados (push/pop del prologue/epilogue) y el
 * numero de spill slots (tamano extra del frame).  Sin dependencias de jit/ ->
 * reutilizable desde cualquier capa del codegen.
 *
 * MODELO DE ASIGNACION (decision de arquitectura, en diseno).  El backend
 * tendra DOS modelos distintos porque responden a PREGUNTAS distintas -- no una
 * estructura vieja con mas campos.  Cada capa responde UNA pregunta (y NINGUNA
 * existente cambia de significado):
 *
 *     LaneAssignment      ¿que lane recibe el valor?
 *     AssignmentPlan           ¿que parte del valor merece vivir en registro?
 *     AllocationTimeline  ¿donde vive el valor en el tiempo?
 *
 * Filosofia del proyecto -- Conocimiento -> Plan -> Transformacion:
 *
 *     Conocimiento:   AllocatorDiagnostics + SpillTaxonomy + execution_weight /
 * hw_cost. Plan:           AssignmentPlan  (lo que producen las
 * transformaciones de asignacion). Transformacion: TimelineBuilder (materializa
 * un AllocationTimeline) + Rewrite.
 *
 * La transformacion de asignacion produce un @c AssignmentPlan; @c
 * TimelineBuilder materializa un @c AllocationTimeline, que constituye el
 * formato TEMPORAL consumido por el Rewrite. La Recovery encaja: produce un
 * AssignmentPlan de 1 tramo (MEM->REG cubriendo toda la vida); el Splitting
 * produce MEM->REG->MEM; el Timeline no sabe cual lo creo (recovered_area lo
 * mide la transformacion, no el Timeline).
 *
 * RegAlloc permanece INMUTABLE: representa una asignacion PLANA (una ubicacion
 * por vreg), el formato que producen los allocators actuales.  TimelineBuilder
 * puede convertirla trivialmente en un AllocationTimeline.  Los modelos
 * TEMPORALES pertenecen EXCLUSIVAMENTE a @c AllocationTimeline -- nunca a esta
 * estructura.  (Contrato arquitectonico.)
 *
 * AllocationTimeline responde una pregunta FUNDAMENTAL -- "¿donde vive el valor
 * en el tiempo?" -- asi que probablemente deje de ser "la estructura del
 * splitting" y se convierta en el IR DE ASIGNACION del backend: el nivel donde
 * convergen Recovery, Splitting, rematerializacion parcial y futuras
 * optimizaciones.
 */

#ifndef VESTA_CODEGEN_REGALLOC_H
#define VESTA_CODEGEN_REGALLOC_H

#include <cstdint>
#include <vector>

namespace codegen {

/**
 * @struct RegAlloc
 * @brief Resultado de la asignacion de registros (vreg -> reg/slot).
 */
struct RegAlloc {
    /// Ubicacion asignada a un vreg.
    enum class Loc : uint8_t {
        NONE = 0, ///< vreg muerto (interval vacio): sin asignacion
        REG = 1,  ///< en un registro fisico (@c reg)
        SPILL = 2 ///< en un spill slot de stack (@c slot)
    };
    struct VAssign {
        Loc loc = Loc::NONE;
        uint8_t reg = 0;   ///< id fisico (valido si loc==REG)
        uint32_t slot = 0; ///< indice de spill slot (valido si loc==SPILL)
    };
    /// Asignacion por vreg id (denso 0..vreg_count-1).
    std::vector<VAssign> assign;
    /// Registros callee-saved usados (deduplicados, orden estable).  El
    /// prologue/epilogue debe push/pop estos.
    std::vector<uint8_t> callee_saved_used;
    /// Numero de spill slots reservados (cada uno @c pointer_size bytes).
    uint32_t num_spill_slots = 0;

    /* ---- Accesores ---- */
    bool in_reg(uint32_t vid) const noexcept {
        return vid < assign.size() && assign[vid].loc == Loc::REG;
    }
    bool spilled(uint32_t vid) const noexcept {
        return vid < assign.size() && assign[vid].loc == Loc::SPILL;
    }
    uint8_t reg_of(uint32_t vid) const noexcept { return assign[vid].reg; }
    uint32_t slot_of(uint32_t vid) const noexcept { return assign[vid].slot; }

    /* ---- Constructores ----
     *
     * Existen para que los productores no repitan el "poner tres campos a la
     * vez": construir una asignacion es parte de la REPRESENTACION, no logica
     * de quien la construye.  Crecen el vector porque los productores de estilo
     * disperso (@c ir::allocate_regs) no conocen el tamano denso de antemano.
     *
     * @note @c loc es UN campo, asi que un valor NO puede estar a la vez en
     *       registro y derramado.  En la representacion dispersa anterior eran
     *       dos mapas independientes y ese estado SI era representable -- fue
     *       un bug real (leia del registro ya reasignado).  Aqui deja de ser
     *       expresable: asignar una ubicacion borra la otra por construccion.
     */
    void ensure(uint32_t vid) {
        if (vid >= assign.size()) assign.resize(static_cast<size_t>(vid) + 1);
    }
    void set_reg(uint32_t vid, uint8_t r) {
        ensure(vid);
        assign[vid].loc = Loc::REG;
        assign[vid].reg = r;
    }
    void set_spill(uint32_t vid, uint32_t s) {
        ensure(vid);
        assign[vid].loc = Loc::SPILL;
        assign[vid].slot = s;
    }
};

} // namespace codegen

#endif // VESTA_CODEGEN_REGALLOC_H
