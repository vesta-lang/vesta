/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/allowed_lanes.h
 * @brief ConstraintBuilder: de los Facts de un valor -> el conjunto de lanes
 *        ADMISIBLES por CORRECTITUD (AllowedLaneSet).  El coloreador consume este
 *        conjunto sin saber QUE significa cada regla -- solo pregunta "¿admisible?".
 *
 *     Facts (ValueRequirements)          PhysicalRegisterBank
 *              \                               /
 *               \                             /
 *                v         lane_admissible   v
 *                 +-----------> {clase, ancho, allocatable, crosses_call} ------+
 *                                                                               |
 *                                                                               v
 *                                                                        AllowedLaneSet
 *                                                                               |
 *                                                                               v
 *                                                                           coloring
 *
 * SEPARACION Knowledge -> Constraints -> Decision (regla del usuario).  Aqui viven
 * las restricciones DURAS de correctitud, NO las preferencias economicas:
 *   - clase / ancho / allocatable / pin  -> "que lanes PUEDEN alojar el valor".
 *   - crosses_call -> PRESERVED           -> "un valor vivo a traves de un CALL solo
 *     puede vivir en callee-saved; una lane volatil la clobbea el CALL".  Es
 *     CORRECTITUD ABI, no una preferencia: por eso es una Constraint dura, no un
 *     termino del Objective.  La alternativa economica (caller-saved pagando
 *     save/restore alrededor del CALL, o spill) es el Paso 4 -- ahi SI entra el
 *     Objective, RELAJANDO esta restriccion a una eleccion coste-beneficio.
 *
 * El coloreador nunca interpreta @c crosses_call: recibe, por valor, el conjunto de
 * lanes en el que puede colorear con normalidad.  Si esta capa mintiera (permitir
 * una lane volatil a un cross-call), el codigo seria INCORRECTO -- por eso la
 * validacion (is_proper_coloring) exige lo mismo que este builder.
 *
 * i18n: produce DATOS.  ADITIVO.
 */

#ifndef VESTA_CODEGEN_RBANK_ALLOWED_LANES_H
#define VESTA_CODEGEN_RBANK_ALLOWED_LANES_H

#include "codegen/rbank/abstract_problem.h"
#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/value_requirements.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @enum LaneHazard
 * @brief Por que una lane NO satisface los REQUISITOS DE UN VALOR (o @c NONE si los
 *        satisface).  UN solo concepto en vez de N categorias ad hoc: el allocator no
 *        distingue GC de EH de inline-asm de ABI -- todas son restricciones del mismo
 *        nivel (el VALOR).  El origen (GC/EH/asm/...) se pierde a proposito: al
 *        coloreador solo le importa el EFECTO.  Nuevos hazards del runtime = nuevo
 *        valor aqui, sin tocar el algoritmo.  NOTA: la interferencia (OVERLAP) NO esta
 *        aqui -- es una propiedad del COLOREADO (entre DOS valores), otro nivel.  DATO
 *        (i18n): se mapea a VXNNNN al emitir.
 */
enum class LaneHazard : uint8_t {
    NONE = 0,          ///< la lane satisface los requisitos del valor.
    WRONG_CLASS,       ///< lane de otra clase de recurso.
    NOT_ALLOCATABLE,   ///< lane reservada/scratch/frame (no asignable).
    UNSUPPORTED_WIDTH, ///< la lane no soporta el ancho del valor.
    MUST_MEMORY,       ///< el valor DEBE residir en memoria (GC/EH/force_spill/addr).
    LANE_FORBIDDEN,    ///< la lane muere en un punto que el valor atraviesa (asm/stub).
    NOT_PRESERVED,     ///< el valor requiere una lane PRESERVADA (cross-call) y no lo es.
    PIN_VIOLATED,      ///< la lane no satisface el pin (fixed_reg) del valor.
};
static constexpr size_t kLaneHazardCount = 8;

/** @brief Nombre corto del LaneHazard (para paneles/diagnostico). */
inline const char *lane_hazard_name(LaneHazard k) {
    switch (k) {
        case LaneHazard::NONE:              return "none";
        case LaneHazard::WRONG_CLASS:       return "class";
        case LaneHazard::NOT_ALLOCATABLE:   return "reserved";
        case LaneHazard::UNSUPPORTED_WIDTH: return "width";
        case LaneHazard::MUST_MEMORY:       return "must_memory";
        case LaneHazard::LANE_FORBIDDEN:    return "lane_forbidden";
        case LaneHazard::NOT_PRESERVED:     return "not_preserved";
        case LaneHazard::PIN_VIOLATED:      return "pin";
    }
    return "?";
}

/**
 * @brief El hazard que hace INADMISIBLE la lane @p lane para el valor @p r, o
 *        @c LaneHazard::NONE si es admisible.  FUENTE UNICA DE VERDAD: tanto el coloreo
 *        (@c lane_admissible) como el validador (@c validate_coloring) preguntan AQUI
 *        -- nunca reinterpretan crosses_call/is_gc por su cuenta.
 *
 * Version PURA (recibe la @c Lane -> cero busquedas; hot path del coloreo).  NO evalua
 * el pin (rama GENERAL): un pin es restriccion de nivel superior (ABI/asm ya fijaron el
 * registro) y lo maneja el coloreo/builder aparte -> @c PIN_VIOLATED no lo devuelve
 * esta funcion.  Orden = prioridad del diagnostico (la PRIMERA razon que falla).
 */
inline LaneHazard lane_hazard(const ValueRequirements &r, const Lane &lane,
                              bool vec_active) {
    // Debe residir en memoria: ninguna lane sirve (GC root cross-call, live-in a un
    // handler abnormal/force_spill, address-taken).  El allocator NO necesita saber el
    // MOTIVO -- solo "no puede vivir en registro".
    if (r.must_be_memory())                        return LaneHazard::MUST_MEMORY;
    if (lane.cls != r.cls)                         return LaneHazard::WRONG_CLASS;
    if (!lane.allocatable(vec_active))             return LaneHazard::NOT_ALLOCATABLE;
    if (!lane.supports(r.width))                   return LaneHazard::UNSUPPORTED_WIDTH;
    // Lane muerta en un punto que el valor atraviesa (clobbers de asm/setjmp/stub/
    // syscall...).  Bitmask precomputado -> el coloreo no sabe el origen.
    if (r.lane_forbidden(lane.id))                 return LaneHazard::LANE_FORBIDDEN;
    // El valor requiere una lane PRESERVADA (esta viva a traves de un CALL que clobbea
    // las volatiles).  El nombre no menciona "caller-saved": el requisito es "preservada".
    if (r.crosses_call && lane.preservation_of(r.width) != SavePolicy::PRESERVED)
        return LaneHazard::NOT_PRESERVED;
    return LaneHazard::NONE;
}

/** @brief ¿La lane @p lane es admisible para @p r?  (@c lane_hazard == NONE). */
inline bool lane_admissible(const ValueRequirements &r, const Lane &lane,
                            bool vec_active) {
    return lane_hazard(r, lane, vec_active) == LaneHazard::NONE;
}

/** @brief Conveniencia por id: hace UN @c by_id y delega en la version pura. */
inline bool lane_admissible(const ValueRequirements &r, uint8_t lane_id,
                            const PhysicalRegisterBank &bank, bool vec_active) {
    const Lane *l = bank.by_id(lane_id);
    return l && lane_admissible(r, *l, vec_active);
}

/**
 * @struct AllowedLanes
 * @brief AllowedLaneSet MATERIALIZADO por valor -- vista de DATOS inspeccionable y
 *        testeable del ConstraintBuilder.  El coloreo puede consultar @c lane_admissible
 *        directo (mismo resultado, sin materializar); esto existe para verlo/testearlo.
 */
struct AllowedLanes {
    std::unordered_map<uint32_t, std::vector<uint8_t>> per_value; ///< value_id -> lanes.

    /** @brief Lanes admisibles de @p value_id (vacio si no esta o no cabe en ninguna). */
    const std::vector<uint8_t> &lanes_of(uint32_t value_id) const {
        static const std::vector<uint8_t> kEmpty;
        auto it = per_value.find(value_id);
        return it == per_value.end() ? kEmpty : it->second;
    }
};

/**
 * @brief Construye el AllowedLaneSet de un problema: por cada valor, las lanes que
 *        satisfacen las restricciones DURAS (via @c lane_admissible; el pin restringe
 *        a su unica lane).  Funcion pura (Facts + banco -> conjunto).
 */
inline AllowedLanes build_allowed_lanes(const AbstractProblem &p,
                                        const PhysicalRegisterBank &bank,
                                        bool vec_active) {
    AllowedLanes al;
    al.per_value.reserve(p.values.size());
    for (const AbstractValue &v : p.values) {
        std::vector<uint8_t> lanes;
        if (v.req.fixed_reg >= 0) {
            // Pin duro: solo esa lane (si es usable y sobrevive a los hazards).
            // Restriccion de NIVEL SUPERIOR al allocator (ABI/asm ya fijaron el
            // registro); prevalece sobre crosses_call pero no sobre debe-memoria ni
            // una lane clobbeada (si su pin muere, el valor es infactible en registro).
            const uint8_t fid = static_cast<uint8_t>(v.req.fixed_reg);
            const Lane *l = bank.by_id(fid);
            if (!v.req.must_be_memory() && !v.req.lane_forbidden(fid) && l &&
                l->cls == v.req.cls && l->allocatable(vec_active) &&
                l->supports(v.req.width))
                lanes.push_back(fid);
        } else {
            for (const Lane &l : bank.lanes)
                if (lane_admissible(v.req, l, vec_active)) lanes.push_back(l.id);
        }
        al.per_value.emplace(v.value_id, std::move(lanes));
    }
    return al;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_ALLOWED_LANES_H
