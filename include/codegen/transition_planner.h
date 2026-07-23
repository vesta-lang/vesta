/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/transition_planner.h
 * @brief @c TransitionPlanner: responde UNA pregunta --
 *
 *     ¿que movimientos exige ESTE punto del programa?
 *
 * Cuando un valor cambia de ubicacion entre dos tramos del @c AllocationTimeline (p.ej.
 * memoria -> registro al entrar en un hueco), hay que MOVERLO.  Este objeto calcula esos
 * movimientos; el Rewrite solo los EMITE.
 *
 * API en PROGRAM POINTS, no en posiciones: el Rewrite recorre INSTRUCCIONES, y su pregunta
 * natural es "¿que emito antes / despues de esta instruccion?", no "¿hay una transicion
 * entre 14 y 15?".  Asi el Rewrite jamas ve segmentos, @c LinearPos ni la convencion
 * temporal, y el dia que cambie la representacion del timeline su API no se mueve:
 *
 *     emit(planner.before_instruction(gi));
 *     lower(instr);
 *     emit(planner.after_instruction(gi));
 *
 * REPARTO (por que existe): materializar los movimientos solo es posible DENTRO del Rewrite
 * -- insertarlos antes desplazaria los indices de instruccion e invalidaria el timeline, que
 * esta definido sobre el MachineIR original.  Pero el Rewrite no debe CALCULARLOS: eso es
 * conocimiento del modelo temporal.  De ahi la separacion:
 *
 *     AllocationTimeline   ¿donde vive un valor en un punto?
 *     AllocationResolver   ¿donde vive ESTE valor aqui?
 *     TransitionPlanner    ¿que movimientos exige este punto?     <-- aqui
 *     Rewrite              emite lo que le entregan (pasivo)
 *
 * Cuando las transiciones crezcan (registro->registro, constante->registro, remat, reparacion
 * de PHI...) crecen AQUI, y el Rewrite no acumula un switch: el sigue emitiendo un movimiento
 * generico "de esta ubicacion a esta otra".  Y este objeto se prueba AISLADO (un timeline
 * MEM|REG|MEM debe producir un movimiento de entrada y otro de salida) sin ejecutar el
 * Rewrite entero.
 */

#ifndef VESTA_CODEGEN_TRANSITION_PLANNER_H
#define VESTA_CODEGEN_TRANSITION_PLANNER_H

#include "codegen/allocation_timeline.h"
#include "codegen/linear_pos.h"
#include "codegen/value_location.h"

#include <cstdint>
#include <vector>

namespace codegen {

/**
 * @struct ValueTransition
 * @brief Un valor debe pasar de @c from a @c to en este punto del programa.  El Rewrite lo
 *        materializa mecanicamente (un movimiento destino <- origen); no interpreta el par.
 */
struct ValueTransition {
    uint32_t      vreg = 0;
    ValueLocation from; ///< donde estaba.
    ValueLocation to;   ///< donde debe estar a partir de aqui.
};

/**
 * @class TransitionPlanner
 * @brief Precalcula, por instruccion, los movimientos que exige el modelo temporal.
 *
 * Sin splitting no hay bordes internos -> todas las consultas devuelven vacio y el Rewrite
 * no emite nada (codigo identico).  El coste de construccion es O(segmentos).
 */
class TransitionPlanner {
  public:
    explicit TransitionPlanner(const AllocationTimeline &tl) {
        /* Alcance: la ultima posicion real del timeline decide cuantas instrucciones hay.
         * Se ignora el centinela "hasta el final" del caso sin rangos (no tiene bordes). */
        uint32_t max_pos = 0;
        for (const ValueLocationTimeline &v : tl.values)
            for (const LocationSegment &s : v.segments)
                if (s.to.is_valid() && s.to.value > max_pos) max_pos = s.to.value;
        const size_t n = static_cast<size_t>(max_pos / 2u) + 2u;
        before_.resize(n);
        after_.resize(n);

        /* Un movimiento nace donde DOS tramos consecutivos del mismo valor difieren en
         * ubicacion y son contiguos (si hay hueco de liveness el valor murio: nada que
         * mover).  La posicion del borde se traduce aqui a un PROGRAM POINT -- esa
         * traduccion es justo lo que el Rewrite no debe conocer. */
        for (const ValueLocationTimeline &v : tl.values) {
            for (size_t i = 1; i < v.segments.size(); ++i) {
                const LocationSegment &prev = v.segments[i - 1];
                const LocationSegment &cur = v.segments[i];
                if (prev.to != cur.from) continue;             // hueco: el valor no vive.
                if (prev.location == cur.location) continue;   // no cambia de sitio.
                const uint32_t pos = cur.from.value;
                const size_t gi = static_cast<size_t>(pos / 2u);
                if (gi >= before_.size()) continue;
                ValueTransition t{v.vreg, prev.location, cur.location};
                if ((pos & 1u) == 0u) before_[gi].push_back(t); // el valor se LEE ya movido.
                else after_[gi].push_back(t);                   // tras definir en esa instr.
                ++total_;
            }
        }
    }

    /// Movimientos a emitir ANTES de la instruccion @p gi.
    const std::vector<ValueTransition> &before_instruction(uint32_t gi) const noexcept {
        return gi < before_.size() ? before_[gi] : empty_;
    }
    /// Movimientos a emitir DESPUES de la instruccion @p gi.
    const std::vector<ValueTransition> &after_instruction(uint32_t gi) const noexcept {
        return gi < after_.size() ? after_[gi] : empty_;
    }
    /// ¿El plan exige algun movimiento?  Permite al consumidor saltarse el trabajo.
    bool empty() const noexcept { return total_ == 0; }

  private:
    std::vector<std::vector<ValueTransition>> before_, after_;
    std::vector<ValueTransition> empty_;
    uint32_t total_ = 0;
};

} // namespace codegen

#endif // VESTA_CODEGEN_TRANSITION_PLANNER_H
