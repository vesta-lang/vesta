/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/constraints.h
 * @brief Nivel 2 del modelo de asignacion de recursos: FEASIBILIDAD DURA.
 *
 * @c ConstraintSet captura las relaciones que TODA asignacion VALIDA debe
 * cumplir (a diferencia del Objective, que son preferencias SUAVES).  Y
 * @c validate_assignment es el primitivo que VERIFICA una solucion: dada una
 * asignacion valor->lane, comprueba que no viola ninguna restriccion ni ningun
 * @c ValueRequirements.  Ese verificador es la espina dorsal del allocator:
 * lo usan el round-trip de validacion (Fase 0.5), el generador de candidatos y
 * la @c DecisionPolicy -- ninguna solucion se acepta sin pasar por aqui.
 *
 *     PhysicalRegisterBank  (que ofrece el hardware)
 *             ^
 *     ValueRequirements     (que necesita cada valor)
 *             ^
 *     Constraints           (que combinaciones son FACTIBLES)  <-- este modulo
 *             |
 *     Objective -> OptimizationContext -> DecisionPolicy
 *
 * DURO vs SUAVE (limite deliberado):
 *   - DURO (aqui): interferencia (dos valores vivos a la vez no comparten
 *     lane, considerando ALIASING sub-registro), pin fijo obligado por el ISA
 *     (shift->CL, div->RAX/RDX) o por el usuario (inline asm), operandos que el
 *     encoding obliga a ser distintos o iguales.
 *   - SUAVE (Objective, NO aqui): el "tie" de two-address (dst=src1 en x86) es
 *     una PREFERENCIA de coalescing -- se puede romper con un mov; NO es una
 *     restriccion dura.  Igual las preferencias de callee-saved o de coalesce.
 *
 * DIAGNOSTICOS MULTI-IDIOMA: @c validate_assignment devuelve DATOS
 * (@c ViolationKind + valores/lane implicados), NUNCA un mensaje baked.  El
 * consumidor mapea a un codigo @c VXNNNN del catalogo i18n al emitir.
 *
 * Fase 0: ADITIVO, sin consumidores (solo el test).  El grafo de interferencia
 * EFICIENTE (bitsets para el coloreado) es de Fase 2; aqui el ConstraintSet es
 * la representacion + el verificador, poblado desde liveness en Fase 0.25.
 */

#ifndef VESTA_CODEGEN_RBANK_CONSTRAINTS_H
#define VESTA_CODEGEN_RBANK_CONSTRAINTS_H

#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/value_requirements.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace codegen {
namespace rbank {

/** @brief Lane sentinel: el valor esta en memoria (spilled), no ocupa lane. */
static constexpr int kSpilled = -1;

/**
 * @enum ConstraintKind
 * @brief Tipo de restriccion DURA.
 */
enum class ConstraintKind : uint8_t {
    INTERFERE,       ///< @c a y @c b vivos a la vez: no comparten lane (ni alias).
    DIFFERENT_LANE,  ///< @c a y @c b deben estar en lanes distintas (encoding).
    SAME_LANE,       ///< @c a y @c b DEBEN compartir lane (operando tied duro).
    FIXED_LANE,      ///< @c a fijado a la lane @c lane (shift->CL, div->RAX, pin).
};

/**
 * @struct Constraint
 * @brief Una restriccion dura entre valores (o valor-lane para FIXED_LANE).
 */
struct Constraint {
    ConstraintKind kind;
    uint32_t       a    = 0;   ///< primer valor implicado.
    uint32_t       b    = 0;   ///< segundo valor (INTERFERE/DIFFERENT/SAME).
    uint8_t        lane = 0;   ///< lane fisica (solo FIXED_LANE).
};

/**
 * @struct ConstraintSet
 * @brief Conjunto de restricciones duras de una funcion.
 */
struct ConstraintSet {
    std::vector<Constraint> items;

    void interfere(uint32_t a, uint32_t b) {
        items.push_back({ConstraintKind::INTERFERE, a, b, 0});
    }
    void different_lane(uint32_t a, uint32_t b) {
        items.push_back({ConstraintKind::DIFFERENT_LANE, a, b, 0});
    }
    void same_lane(uint32_t a, uint32_t b) {
        items.push_back({ConstraintKind::SAME_LANE, a, b, 0});
    }
    void fixed_lane(uint32_t a, uint8_t lane) {
        items.push_back({ConstraintKind::FIXED_LANE, a, 0, lane});
    }

    /**
     * @brief ¿Tienen @p a y @p b que ir en lanes DISTINTAS?
     *
     * No es lo mismo que interferir.  Dos valores que no se solapan en el
     * tiempo pueden compartir lane sin problema, pero a veces la FORMA de la
     * instruccion lo prohibe igual: en una operacion de dos operandos no
     * conmutativa (`dst = dst OP src`), si al segundo operando le toca la lane
     * del destino, hay que salvarlo antes de pisarlo -- dos movimientos y un
     * temporal que no harian falta si el asignador no lo hubiera juntado.
     *
     * @param a Primer valor.
     * @param b Segundo valor.
     * @return true si no pueden compartir lane.
     */
    bool must_differ(uint32_t a, uint32_t b) const {
        for (const Constraint &c : items)
            if (c.kind == ConstraintKind::DIFFERENT_LANE &&
                ((c.a == a && c.b == b) || (c.a == b && c.b == a)))
                return true;
        return false;
    }

    /** @brief True si @p a y @p b tienen una arista INTERFERE (lineal, Fase 0). */
    bool interferes(uint32_t a, uint32_t b) const {
        for (const Constraint &c : items)
            if (c.kind == ConstraintKind::INTERFERE &&
                ((c.a == a && c.b == b) || (c.a == b && c.b == a)))
                return true;
        return false;
    }
};

/**
 * @struct LaneAssignment
 * @brief Asignacion valor -> lane fisica (o @c kSpilled si en memoria).
 */
struct LaneAssignment {
    std::unordered_map<uint32_t, int> lane; ///< value_id -> lane id, o kSpilled.

    void assign(uint32_t value_id, int lane_id) { lane[value_id] = lane_id; }
    void spill(uint32_t value_id) { lane[value_id] = kSpilled; }
    /** @brief Lane asignada a @p value_id, o @c kSpilled si no esta o spilled. */
    int lane_of(uint32_t value_id) const {
        auto it = lane.find(value_id);
        return it == lane.end() ? kSpilled : it->second;
    }
};

/**
 * @enum ViolationKind
 * @brief Motivo (DATO) por el que una asignacion viola la feasibilidad.  Se
 *        mapea a un codigo @c VXNNNN del catalogo i18n al EMITIR; no es texto.
 */
enum class ViolationKind : uint8_t {
    NONE = 0,                 ///< asignacion valida.
    INTERFERENCE_OVERLAP,     ///< @c a y @c b interfieren y sus lanes aliasan.
    DIFFERENT_LANE_VIOLATED,  ///< @c a y @c b comparten lane pero deben diferir.
    SAME_LANE_VIOLATED,       ///< @c a y @c b en lanes distintas pero deben coincidir.
    FIXED_LANE_VIOLATED,      ///< @c a no esta en la lane fijada.
    REQUIREMENT_UNSAT,        ///< la lane asignada no satisface el ValueRequirements.
};

/**
 * @struct ConstraintViolation
 * @brief Resultado de @c validate_assignment: DATOS para el diagnostico i18n.
 */
struct ConstraintViolation {
    bool          ok     = true;                 ///< asignacion valida.
    ViolationKind kind   = ViolationKind::NONE;  ///< motivo (dato).
    uint32_t      a      = 0;                    ///< valor implicado.
    uint32_t      b      = 0;                    ///< segundo valor (si aplica).
    int           lane   = kSpilled;             ///< lane implicada (si aplica).
    /// Si @c kind == REQUIREMENT_UNSAT, el motivo especifico del requisito.
    UnsatReason   unsat  = UnsatReason::OK;
};

/**
 * @brief Verifica que @p assign satisface TODAS las restricciones de @p cs y
 *        los @p reqs sobre el banco @p bank.  Devuelve la PRIMERA violacion
 *        como DATO (o @c ok=true).
 * @param reqs  requisitos por valor (para comprobar clase/ancho/usabilidad de
 *              la lane asignada).  Puede estar vacio (solo valida constraints).
 * @param vec_reduction_active  si el path de reduccion vectorial esta activo.
 *
 * Interferencia: dos valores con arista INTERFERE, ambos en registro, cuyas
 * lanes ALIASAN (via @c AliasSet -> captura sub-registro), violan.  Un valor
 * spilled (@c kSpilled) no ocupa lane y nunca interfiere.
 */
inline ConstraintViolation validate_assignment(
    const ConstraintSet &cs, const LaneAssignment &assign,
    const std::vector<ValueRequirements> &reqs,
    const PhysicalRegisterBank &bank, bool vec_reduction_active) {

    ConstraintViolation v;

    // 1) Restricciones entre valores / valor-lane.
    for (const Constraint &c : cs.items) {
        const int la = assign.lane_of(c.a);
        switch (c.kind) {
        case ConstraintKind::INTERFERE: {
            const int lb = assign.lane_of(c.b);
            if (la == kSpilled || lb == kSpilled) break; // spilled no interfiere.
            const AliasSet *aa = bank.aliases_of(static_cast<uint8_t>(la));
            const AliasSet *ab = bank.aliases_of(static_cast<uint8_t>(lb));
            if (aa && ab && aa->overlaps(*ab)) {
                v = {false, ViolationKind::INTERFERENCE_OVERLAP, c.a, c.b, la, UnsatReason::OK};
                return v;
            }
            break;
        }
        case ConstraintKind::DIFFERENT_LANE: {
            const int lb = assign.lane_of(c.b);
            if (la != kSpilled && la == lb) {
                v = {false, ViolationKind::DIFFERENT_LANE_VIOLATED, c.a, c.b, la, UnsatReason::OK};
                return v;
            }
            break;
        }
        case ConstraintKind::SAME_LANE: {
            const int lb = assign.lane_of(c.b);
            if (la != lb) {
                v = {false, ViolationKind::SAME_LANE_VIOLATED, c.a, c.b, la, UnsatReason::OK};
                return v;
            }
            break;
        }
        case ConstraintKind::FIXED_LANE: {
            if (la != kSpilled && la != static_cast<int>(c.lane)) {
                v = {false, ViolationKind::FIXED_LANE_VIOLATED, c.a, 0, la, UnsatReason::OK};
                return v;
            }
            break;
        }
        }
    }

    // 2) La lane asignada a cada valor debe satisfacer su ValueRequirements.
    for (const ValueRequirements &r : reqs) {
        const int l = assign.lane_of(r.value_id);
        if (l == kSpilled) continue; // en memoria: lo cubre la residencia.
        ValueRequirements probe = r;
        probe.fixed_reg = static_cast<int16_t>(l); // "¿es valida esta lane?"
        SatisfiabilityReport sat =
            requirements_satisfiable(probe, bank, vec_reduction_active);
        if (!sat.ok) {
            v = {false, ViolationKind::REQUIREMENT_UNSAT, r.value_id, 0, l, sat.reason};
            return v;
        }
    }

    return v; // ok.
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_CONSTRAINTS_H
