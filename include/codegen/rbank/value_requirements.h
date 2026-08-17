/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/value_requirements.h
 * @brief Nivel 1 del modelo de asignacion de recursos: QUE NECESITA un valor.
 *
 * @c ValueRequirements describe, por cada valor SSA que debe materializarse, lo
 * que REQUIERE del banco fisico: su clase de recurso, el ancho que ocupa, donde
 * puede vivir (registro/memoria) y los HECHOS estructurales que condicionan su
 * asignacion (cruza un CALL, es un handle GC, se toma su direccion, es
 * rematerializable, profundidad de loop, pin duro a un registro).
 *
 * Es la PRIMERA pieza de la piramide encima del @c PhysicalRegisterBank:
 *
 *     PhysicalRegisterBank   (que ofrece el hardware)
 *             ^
 *     ValueRequirements      (que necesita cada valor)  <-- este modulo
 *             |
 *     Constraints -> Objective -> OptimizationContext -> DecisionPolicy
 *
 * SEPARACION DE ROLES (importante):
 *   - ValueRequirements = HECHOS/NECESIDADES del valor.  NO decide nada.
 *   - Los HECHOS estructurales (@c crosses_call, @c loop_depth, ...) se POBLAN
 *     desde la capa de Facts (AnalysisManager/liveness/loopinfo); NO se
 *     recomputan aqui.  Este modulo solo los TRANSPORTA hacia el allocator.
 *   - Interpretar un hecho como PREFERENCIA (p.ej. "cruza CALL -> prefiere
 *     callee-saved") es trabajo del Objective, no de aqui.
 *
 * DIAGNOSTICOS MULTI-IDIOMA (regla del proyecto): la comprobacion de
 * satisfacibilidad devuelve DATOS (@c UnsatReason + args), NUNCA un mensaje
 * baked.  Cuando un consumidor deba EMITIR un diagnostico al usuario, mapea el
 * @c UnsatReason a un codigo estable @c VXNNNN del catalogo i18n
 * (@c diags_.diag(loc, lvl, VXNNNN, args)).  Este modulo no formatea texto.
 *
 * Fase 0: ADITIVO, sin consumidores (solo el test).  DATA pura + funciones
 * puras -> serializable, sin comportamiento oculto.
 */

#ifndef VESTA_CODEGEN_RBANK_VALUE_REQUIREMENTS_H
#define VESTA_CODEGEN_RBANK_VALUE_REQUIREMENTS_H

#include "codegen/rbank/physical_bank.h"

#include <cstdint>

namespace codegen {
namespace rbank {

/**
 * @enum Residency
 * @brief Donde debe poder vivir el valor.
 */
enum class Residency : uint8_t {
    ANY = 0,   ///< registro o memoria: el allocator elige.
    REGISTER,  ///< debe estar en registro en sus usos (escalar normal).
    MEMORY,    ///< debe tener direccion en memoria (address-taken, agregados).
};

/**
 * @struct ValueRequirements
 * @brief Necesidades de un valor SSA de cara al allocator.
 *
 * @c cls y @c width son del MODELO (ResourceClass/ViewWidth), no de una ISA.
 * @c fixed_reg es el UNICO campo target-especifico: un id fisico del banco
 * (o -1 si no hay pin).  Los booleanos son HECHOS poblados desde la capa de
 * Facts, no recomputados aqui.
 */
struct ValueRequirements {
    uint32_t      value_id  = 0;                  ///< id del valor SSA.
    ResourceClass cls       = ResourceClass::GP;  ///< clase de recurso que necesita.
    ViewWidth     width     = ViewWidth::W8;       ///< ancho que ocupa.
    Residency     residency = Residency::ANY;      ///< donde puede vivir.

    // --- Hechos estructurales (poblados desde la capa de Facts) ---
    /* DOS preguntas distintas sobre la misma llamada, con criterios OPUESTOS.
     * Tenerlas en un solo campo era un bug: el criterio bueno para una es el
     * malo para la otra (medido; ver la nota de @c liveness_adapter).
     *
     * @c crosses_call = VIVO EN la posicion de un CALL (criterio INCLUSIVO,
     * `def <= p <= end`).  Es lo que necesita el stackmap: una raiz de GC viva en
     * un safepoint DEBE constar, y estrechar esto deja de marcar raices -- no es
     * rendimiento, es correccion.
     *
     * @c needs_preserved = SOBREVIVE a la llamada (criterio ESTRICTO,
     * `def < p < end`).  Es lo que decide si hace falta una lane PRESERVADA: un
     * valor definido EN `p` es el resultado de `p` y no puede destruirlo quien lo
     * produce; uno cuyo ultimo uso esta EN `p` lo consume `p` y no necesita
     * sobrevivirla.  Con el criterio inclusivo, el operando de un bloque asm
     * -- intervalo [p,p] del propio bloque, que cuenta como posicion de llamada
     * porque clobbea -- salia "cruzandose a si mismo", pedia lane preservada, y
     * en el banco vectorial de x86-64 no hay ninguna: 0 admisibles. */
    bool     crosses_call     = false; ///< vivo EN un CALL (inclusivo) -- GC.
    bool     needs_preserved  = false; ///< sobrevive a un CALL (estricto) -- lane.
    bool     is_gc            = false; ///< handle/puntero GC (visible en stackmap).
    bool     address_taken    = false; ///< se toma su direccion -> implica MEMORY.
    bool     rematerializable = false; ///< recomputable en vez de spillear (const/lea).
    uint16_t loop_depth       = 0;     ///< profundidad de loop de su def (hotness estatica).
    /// Peso de ejecucion MEDIDO (de ProfileFacts).  0 = sin perfil -> el contexto
    /// usa el estimador estatico @c static_execution_weight(loop_depth).
    double   execution_weight = 0.0;

    /// Pin DURO a un registro fisico (inline asm / arg/ret del ABI): id o -1.
    int16_t fixed_reg = -1;

    /// Lanes fisicas PROHIBIDAS para este valor (bitmask por id, 0..63): alguna
    /// muere en un punto que el valor atraviesa (caller-saved de un CALL, clobbers
    /// de un INLINE_ASM, de un stub/syscall/trampolin...).  Al allocator le da igual
    /// QUE lo causo: solo "esta lane no sobrevive".  Es la SEMILLA del futuro
    /// ClobberPoint/HazardPoint unificado: hoy se PRE-COMPUTA por valor (union de los
    /// clobbers en su rango); manyana un vector<ClobberPoint> con posiciones lo
    /// derivara y @c crosses_call se reexpresara como "caller-saved en forbidden".
    uint64_t forbidden_lanes = 0;

    bool has_fixed_reg() const noexcept { return fixed_reg >= 0; }
    /** @brief True si la lane fisica @p id esta prohibida (algun hazard la mata). */
    bool lane_forbidden(uint8_t id) const noexcept {
        return id < 64 && (forbidden_lanes & (1ull << id)) != 0;
    }
    /** @brief True si el valor DEBE residir en memoria (residency o addr-taken). */
    bool must_be_memory() const noexcept {
        return residency == Residency::MEMORY || address_taken;
    }
};

// ---------------------------------------------------------------------------
//  Derivacion pura (tipo-abstracto -> clase / ancho).  Sin dependencia del IR:
//  el mapeo IrType -> ValueRequirements se hace en Fase 0.25 (wiring), usando
//  estos helpers para no acoplar este nivel al frontend/IR.
// ---------------------------------------------------------------------------

/** @brief Clase de recurso segun si el valor es float/vector o entero/puntero. */
constexpr ResourceClass resource_class_for(bool is_fp_or_vector) noexcept {
    return is_fp_or_vector ? ResourceClass::FP_VECTOR : ResourceClass::GP;
}

/**
 * @brief Ancho de vista que ocupa un valor de @p bytes.
 *
 * CONTRATO (total, sin fallos): redondea SIEMPRE HACIA ARRIBA a la potencia de 2
 * mas cercana >= @p bytes, acotado a [W1, W64].  Nunca asserta ni devuelve error.
 * Casos:  0 -> W1;  3 -> W4 (un valor de 3 B ocupa un slot de 4 B);  5..8 -> W8;
 * >64 -> W64 (saturado).  Redondear ARRIBA es lo seguro: nunca sub-dimensiona el
 * registro/slot que aloja el valor.
 */
constexpr ViewWidth view_width_for_bytes(uint32_t bytes) noexcept {
    if (bytes <= 1)  return ViewWidth::W1;
    if (bytes <= 2)  return ViewWidth::W2;
    if (bytes <= 4)  return ViewWidth::W4;
    if (bytes <= 8)  return ViewWidth::W8;
    if (bytes <= 16) return ViewWidth::W16;
    if (bytes <= 32) return ViewWidth::W32;
    return ViewWidth::W64;
}

// ---------------------------------------------------------------------------
//  Satisfacibilidad: puede el banco alojar este requisito.  Devuelve DATOS
//  (i18n-ready), nunca un mensaje.
// ---------------------------------------------------------------------------

/**
 * @enum UnsatReason
 * @brief Motivo (DATO) por el que un @c ValueRequirements NO es satisfacible en
 *        un banco.  Se mapea a un codigo @c VXNNNN del catalogo i18n al EMITIR
 *        un diagnostico; este enum NO es texto.
 */
enum class UnsatReason : uint8_t {
    OK = 0,                       ///< satisfacible.
    NO_LANE_OF_CLASS,             ///< el banco no tiene lanes de la clase pedida.
    WIDTH_UNSUPPORTED,            ///< ninguna lane de la clase soporta el ancho.
    FIXED_REG_MISSING,            ///< el fixed_reg no existe en el banco.
    FIXED_REG_WRONG_CLASS,        ///< el fixed_reg es de otra clase de recurso.
    FIXED_REG_WIDTH_UNSUPPORTED,  ///< el fixed_reg no soporta el ancho pedido.
    FIXED_REG_UNUSABLE,           ///< el fixed_reg no puede alojar un valor
                                  ///< (scratch/frame/puntero-ABI/plataforma).
};

/**
 * @struct SatisfiabilityReport
 * @brief Resultado de @c requirements_satisfiable: DATOS para el diagnostico.
 */
struct SatisfiabilityReport {
    bool        ok            = true;             ///< satisfacible.
    UnsatReason reason        = UnsatReason::OK;  ///< motivo (dato i18n-ready).
    uint8_t     offending_reg = 0xFF;            ///< fixed_reg implicado (o 0xFF).
};

/**
 * @brief Comprueba si el banco @p bank puede alojar el requisito @p r.
 * @param vec_reduction_active  si la funcion usa el path de reduccion vectorial
 *        (mantiene ocupados los VEC_ACC demand-driven).
 *
 * Un valor con residencia en MEMORIA siempre es satisfacible (un slot de pila
 * siempre existe).  Un pin duro (@c fixed_reg) exige que esa lane exista, sea de
 * la clase correcta, soporte el ancho y pueda alojar valores (no scratch/frame).
 * En otro caso, basta con que exista alguna lane asignable de la clase que
 * soporte el ancho.
 */
inline SatisfiabilityReport requirements_satisfiable(
    const ValueRequirements &r, const PhysicalRegisterBank &bank,
    bool vec_reduction_active) {

    SatisfiabilityReport rep;

    // Un valor que reside en memoria no necesita registro: siempre satisfacible.
    if (r.must_be_memory() && !r.has_fixed_reg()) return rep;

    // Pin duro a un registro fisico concreto.
    if (r.has_fixed_reg()) {
        const uint8_t id = static_cast<uint8_t>(r.fixed_reg);
        rep.offending_reg = id;
        const Lane *l = bank.by_id(id);
        if (!l) {
            rep.ok = false; rep.reason = UnsatReason::FIXED_REG_MISSING; return rep;
        }
        if (l->cls != r.cls) {
            rep.ok = false; rep.reason = UnsatReason::FIXED_REG_WRONG_CLASS; return rep;
        }
        if (!l->supports(r.width)) {
            rep.ok = false; rep.reason = UnsatReason::FIXED_REG_WIDTH_UNSUPPORTED; return rep;
        }
        // Un scratch, frame, puntero-ABI o plataforma no puede alojar un valor.
        const ReserveReason rr = l->reserve.reason;
        if (rr == ReserveReason::SCRATCH || rr == ReserveReason::ABI ||
            rr == ReserveReason::PLATFORM) {
            rep.ok = false; rep.reason = UnsatReason::FIXED_REG_UNUSABLE; return rep;
        }
        return rep; // pin valido.
    }

    // Caso general: alguna lane asignable de la clase soporta el ancho.
    if (!bank.can_hold(r.cls, r.width, vec_reduction_active)) {
        rep.ok = false;
        rep.reason = (bank.lane_count(r.cls) == 0)
                         ? UnsatReason::NO_LANE_OF_CLASS
                         : UnsatReason::WIDTH_UNSUPPORTED;
    }
    return rep;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_VALUE_REQUIREMENTS_H
