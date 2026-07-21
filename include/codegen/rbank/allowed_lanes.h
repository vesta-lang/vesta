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
 * @brief ¿La lane @p lane es ADMISIBLE (correctitud DURA) para el valor @p r?
 *        Version PURA: recibe la @c Lane -> cero busquedas (usar cuando ya iteras
 *        @c bank.lanes; es el hot path del coloreo).
 *
 * Reglas ESTATICAS de correctitud (no dinamicas -- @c lane_free lo comprueba el
 * coloreo aparte):
 *   1. clase de recurso correcta,
 *   2. asignable (no scratch/frame/reservada; respeta VEC_ACC demand-driven),
 *   3. soporta el ancho del valor,
 *   4. CONSTRAINT ABI: si @p r cruza un CALL, la lane debe ser PRESERVED
 *      (callee-saved) -- una lane VOLATILE la destruye el CALL.
 *
 * NO evalua el pin (@c fixed_reg): un pin es una restriccion de NIVEL SUPERIOR al
 * allocator (el ABI, una calling convention concreta o el inline-asm ya fijaron ese
 * registro), asi que el allocator no tiene libertad y el pin prevalece sobre
 * @c crosses_call -- no es una excepcion arbitraria, es que la eleccion ya no es del
 * allocator.  El pin lo maneja el coloreo/builder en su rama propia; esta funcion es
 * la rama GENERAL (sin pin).
 */
inline bool lane_admissible(const ValueRequirements &r, const Lane &lane,
                            bool vec_active) {
    if (lane.cls != r.cls) return false;                    // (1) clase.
    if (!lane.allocatable(vec_active)) return false;        // (2) asignable.
    if (!lane.supports(r.width)) return false;              // (3) ancho.
    // (4) CORRECTITUD ABI: cross-call -> solo callee-saved (PRESERVED).
    if (r.crosses_call && lane.preservation_of(r.width) != SavePolicy::PRESERVED)
        return false;
    return true;
}

/**
 * @brief Conveniencia por id: hace UN @c by_id y delega en la version pura.  Para
 *        callers que solo tienen el id fisico (filtro de victimas, validacion).
 */
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
            // Pin duro: solo esa lane (si es usable).  Restriccion de NIVEL SUPERIOR
            // al allocator (ABI/calling-convention/inline-asm ya fijaron el registro),
            // por eso prevalece sobre crosses_call -- el allocator no tiene libertad.
            const uint8_t fid = static_cast<uint8_t>(v.req.fixed_reg);
            const Lane *l = bank.by_id(fid);
            if (l && l->cls == v.req.cls && l->allocatable(vec_active) &&
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
