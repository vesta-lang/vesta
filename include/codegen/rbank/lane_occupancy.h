/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/lane_occupancy.h
 * @brief @c LaneOccupancy: responde UNA pregunta --
 *
 *     ¿esta LIBRE esta lane durante este tramo?
 *
 * Es el CONOCIMIENTO que comparten los tres consumidores de la ocupacion
 * fisica, que hasta ahora lo recalculaban cada uno por su cuenta (misma logica,
 * tres copias):
 *
 *     recover_spills          ¿hay una lane libre en TODO el intervalo?   ->
 * is_free classify_spills         ¿cuanto tiempo libre hay como MUCHO? ->
 * free_time FragmentationRecovery   ¿DONDE estan los huecos libres? ->
 * free_windows
 *
 * Una sola definicion de "libre" -> imposible que dos pasadas discrepen (que la
 * taxonomia diga "hay hueco" y el splitting no lo encuentre, o al reves).
 *
 * ALIASING: una lane no esta libre si la ocupa OTRA que solape con ella en el
 * banco (p.ej. EAX ocupa RAX).  Se consulta @c AliasSet, no el id.
 *
 * CONSERVADOR: usa el ENVOLVENTE [start,end] del valor, que AGRANDA su vida ->
 * sobre-estima la ocupacion y jamas declara libre algo que no lo esta.  Puede
 * perder oportunidades (huecos reales dentro del envolvente); nunca produce
 * codigo incorrecto.  Desaparece cuando @c AbstractValue sea multi-segmento
 * (ver TODO(RangeSet) en backend_bridge).
 *
 * MUTABLE a proposito: quien COMPROMETE una lane (la Recovery, el splitting) la
 * ocupa con
 * @c occupy y las consultas siguientes ya la ven ocupada -> dos decisiones de
 * la misma pasada no pueden pisarse.
 *
 * INVARIANTE: la ocupacion de CADA lane se guarda NORMALIZADA -- ordenada,
 * disjunta y con los tramos contiguos fusionados.  Dos representaciones
 * distintas de la misma ocupacion
 * ([10,20]+[21,25] vs [10,25]) son imposibles, y el numero de tramos no crece
 * con el de decisiones (@c occupy fusiona en vez de acumular).  Es la propiedad
 * que mantiene estable el coste cuando el splitting empiece a comprometer
 * muchos huecos.  Consecuencia: la consulta de una lane es un barrido lineal
 * sobre una lista ya ordenada.
 *
 * i18n: produce DATOS (booleanos/tramos), no diagnosticos -> sin catalogo.
 */

#ifndef VESTA_CODEGEN_RBANK_LANE_OCCUPANCY_H
#define VESTA_CODEGEN_RBANK_LANE_OCCUPANCY_H

#include "codegen/rbank/abstract_problem.h"
#include "codegen/rbank/constraints.h"
#include "codegen/rbank/physical_bank.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace codegen {
namespace rbank {

/**
 * @struct PosRange
 * @brief Tramo CERRADO [from,to] de posiciones lineales.  Cerrado (no
 * semiabierto) porque es la convencion de @c AbstractValue (start/end
 * inclusive) y este objeto vive entero en ese espacio; la traduccion a
 * semiabierto la hace quien produce el plan.
 */
struct PosRange {
    uint32_t from = 0;
    uint32_t to = 0; ///< inclusive.
    uint64_t length() const noexcept {
        return static_cast<uint64_t>(to) - from + 1;
    }
};

/**
 * @brief Inserta @p r en una lista NORMALIZADA (ordenada, disjunta, fusionada)
 * y la deja normalizada.  Los tramos contiguos ([10,20] y [21,25]) se fusionan:
 * representan la misma ocupacion, y no distinguirlos es lo que mantiene la
 * lista acotada.
 */
inline void insert_merged(std::vector<PosRange> &list, PosRange r) {
    // Absorbe todo lo que solape o toque a r; el resto conserva su orden.
    std::vector<PosRange> out;
    out.reserve(list.size() + 1);
    bool placed = false;
    for (const PosRange &x : list) {
        const bool
            touches = // solapan o son contiguos (con guarda de desbordamiento).
            !(x.to != UINT32_MAX && x.to + 1 < r.from) &&
            !(r.to != UINT32_MAX && r.to + 1 < x.from);
        if (touches) {
            r.from = std::min(r.from, x.from);
            r.to = std::max(r.to, x.to);
            continue;
        }
        if (!placed && r.to < x.from) {
            out.push_back(r);
            placed = true;
        }
        out.push_back(x);
    }
    if (!placed) out.push_back(r);
    list.swap(out);
}

/**
 * @class LaneOccupancy
 * @brief Ocupacion fisica por lane, consultable por tramos.
 */
class LaneOccupancy {
  public:
    /// Ocupacion VACIA sobre el banco @p bank.  El objeto solo entiende de
    /// (lane, tramo): NO conoce valores, ni asignaciones, ni de donde sale la
    /// ocupacion.  Poblarla es tarea de un adaptador (ver @c lane_occupancy_of)
    /// -- asi construirla desde una
    /// @c RegAlloc, un timeline o un test no exige tocar esta clase.
    explicit LaneOccupancy(const PhysicalRegisterBank &bank) {
        for (const Lane &l : bank.lanes)
            if (l.id > max_id_) max_id_ = l.id;
        occ_.resize(static_cast<size_t>(max_id_) +
                    1); // array contiguo: pocos ids.
        // Alias por id, resuelto UNA vez.  Un hueco (nullptr) marca una lane
        // que el banco NO sabe describir; ese caso se trata explicitamente en
        // las consultas.
        alias_.assign(static_cast<size_t>(max_id_) + 1, nullptr);
        for (uint32_t id = 0; id <= max_id_; ++id)
            alias_[id] = bank.aliases_of(static_cast<uint8_t>(id));
    }

    /// Marca @p r como ocupado en @p lane (quien decide, lo reserva).  Mantiene
    /// el invariante: la lista de la lane sigue ordenada, disjunta y fusionada.
    /// Un id fuera del banco se registra igual (crece el array): perder una
    /// ocupacion seria fail-open, y este objeto nunca debe olvidar algo que
    /// esta ocupado.
    void occupy(uint8_t lane, PosRange r) {
        if (lane > max_id_) {
            max_id_ = lane;
            occ_.resize(static_cast<size_t>(max_id_) + 1);
            alias_.resize(static_cast<size_t>(max_id_) + 1, nullptr);
        }
        insert_merged(occ_[lane], r);
    }

    /// ¿Ni un solo valor (ni aliasado) ocupa @p lane dentro de @p r?  Una lane
    /// que el banco no describe NUNCA esta libre: no se puede usar lo que no se
    /// sabe describir.
    bool is_free(uint8_t lane, PosRange r) const {
        if (!describable(lane)) return false;
        bool free_all = true;
        for_each_overlap(lane, r, [&](PosRange) { free_all = false; });
        return free_all;
    }

    /**
     * @brief Tramos LIBRES de @p lane dentro de @p r, ordenados y disjuntos. Es
     * el complemento de la ocupacion y la consulta MAS RICA de la API -- la que
     *        necesita el splitting, que no pregunta "¿esta libre?" sino "¿DONDE
     * lo esta?". (La primitiva comun de las tres consultas es @c
     * for_each_overlap: ahi vive la semantica de "ocupado", incluido el
     * aliasing, en un solo sitio.)
     *
     * La union sobre lanes ALIASADAS puede llegar desordenada aunque cada lane
     * este normalizada (son listas distintas), asi que el barrido la ordena y
     * tolera solapes.
     */
    void free_windows(uint8_t lane, PosRange r,
                      std::vector<PosRange> &out) const {
        out.clear();
        if (!describable(lane)) return; // sin huecos que ofrecer: no es usable.
        std::vector<PosRange> busy;
        for_each_overlap(lane, r, [&](PosRange h) { busy.push_back(h); });
        if (busy.empty()) {
            if (r.from <= r.to) out.push_back(r);
            return;
        }
        std::sort(busy.begin(), busy.end(),
                  [](const PosRange &a, const PosRange &b) {
                      return a.from < b.from;
                  });
        uint32_t cursor = r.from;
        for (const PosRange &b : busy) {
            if (b.from > cursor)
                out.push_back({cursor, b.from - 1}); // hueco antes.
            if (b.to >= cursor) {
                if (b.to >= r.to) return; // ocupado hasta el final.
                cursor = b.to + 1;
            }
        }
        if (cursor <= r.to) out.push_back({cursor, r.to});
    }

    /// Tiempo total LIBRE de @p lane dentro de @p r (el "techo" que mide la
    /// taxonomia).
    uint64_t free_time(uint8_t lane, PosRange r) const {
        std::vector<PosRange> w;
        free_windows(lane, r, w);
        uint64_t t = 0;
        for (const PosRange &x : w)
            t += x.length();
        return t;
    }

  private:
    /// ¿Sabe el banco describir esta lane (tiene @c AliasSet)?  Lo que no se
    /// puede describir no se puede razonar -> no se ofrece nunca como sitio
    /// donde alojar nada.
    bool describable(uint8_t lane) const noexcept {
        return lane <= max_id_ && alias_[lane] != nullptr;
    }

    /// Invoca @p fn con cada tramo OCUPADO (recortado a @p r) que impide usar
    /// @p lane -- incluidos los de las lanes que aliasan con ella.  El nombre
    /// dice lo que hace: recorrer los solapes efectivos, no "aciertos" de una
    /// busqueda. PRECONDICION: @p lane es describible (lo garantizan las
    /// consultas publicas).
    template <typename F>
    void for_each_overlap(uint8_t lane, PosRange r, F fn) const {
        const AliasSet *ls = alias_[lane];
        for (uint32_t id = 0; id <= max_id_; ++id) {
            if (occ_[id].empty()) continue;
            const AliasSet *os = alias_[id];
            /* FAIL-SAFE (propiedad de seguridad, no un detalle): hay algo
             * ocupando una lane que el banco NO sabe describir.  Como no se
             * puede demostrar que no toque a @p lane, se asume que CHOCA:
             *
             *     "si no puedo demostrar que una lane esta libre, esta ocupada"
             *
             * Un alias olvidado (una lane nueva, una vista nueva, otra ISA, un
             * banco a medio inicializar) degrada asi el RENDIMIENTO -- se
             * pierde un hueco -- pero nunca la CORRECTITUD.  Al reves seria un
             * fallo silencioso: planificar un tramo sobre una lane realmente
             * ocupada.
             *
             * La degradacion es ADEMAS local en el tiempo: solo alcanza a los
             * tramos que esa ocupacion desconocida cubre de verdad -- no se
             * inventa ocupacion donde no hay ninguna.  Y las lanes que el banco
             * SI describe siguen razonandose con precision.  Verificado en el
             * test. */
            const bool conflicts = os ? ls->overlaps(*os) : true;
            if (!conflicts) continue; // no aliasa con lane.
            for (const PosRange &o : occ_[id])
                if (r.from <= o.to && o.from <= r.to)
                    fn(PosRange{std::max(o.from, r.from),
                                std::min(o.to, r.to)});
        }
    }

    uint32_t max_id_ = 0;
    /**
     * Alias por id, resuelto una vez en la construccion.
     *
     * INVARIANTE (leer antes de tocar cualquier bucle sobre este vector):
     *
     *     alias_[id] == nullptr   significa   "el banco NO describe esa lane"
     *     alias_[id] == nullptr   NO significa "esa lane no aliasa"
     *
     * Confundir ambas cosas es exactamente el bug que este objeto tuvo:
     * escribir
     * @c if (!alias_[id]) continue; convierte "no lo se" en "no molesta" --
     * fail-OPEN -- y da por libre una lane que puede estar ocupada.  Las dos
     * apariciones legitimas del nullptr son @c describable (la lane consultada
     * no es asignable) y el conflicto conservador de @c for_each_overlap (la
     * ocupacion desconocida bloquea).
     */
    std::vector<const AliasSet *> alias_;
    std::vector<std::vector<PosRange>> occ_;
};

/**
 * @brief ADAPTADOR: ocupacion derivada de una @c LaneAssignment (los valores
 * que YA tienen lane ocupan su vida envolvente).  Vive FUERA de la clase a
 * proposito -- traducir "de donde sale la ocupacion" es responsabilidad de
 * quien la tiene, no del objeto que responde por ella.  Otras fuentes (una @c
 * RegAlloc, un timeline, un test) anyaden SU adaptador sin tocar @c
 * LaneOccupancy.
 */
inline LaneOccupancy lane_occupancy_of(const AbstractProblem &p,
                                       const LaneAssignment &la,
                                       const PhysicalRegisterBank &bank) {
    LaneOccupancy occ(bank);
    for (const AbstractValue &v : p.values) {
        const int lane = la.lane_of(v.value_id);
        if (lane != kSpilled && lane >= 0 && lane <= 0xFF)
            occ.occupy(static_cast<uint8_t>(lane), PosRange{v.start, v.end});
    }
    return occ;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_LANE_OCCUPANCY_H
