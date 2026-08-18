/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ir/linear_pos.h
 * @brief @c ir::LinearPos: posicion lineal en el dominio del IR (la numeracion
 * de
 *        @c compute_liveness -- 1 posicion por instruccion, PHI en block_end
 * del predecesor).
 *
 * TIPO FUERTE, a proposito.  Es un @c uint32_t envuelto que NO se convierte
 * implicitamente a/desde @c uint32_t NI a @c codegen::LinearPos (el dominio
 * MachineIR, 2 posiciones por instruccion).  Asi el compilador RECHAZA mezclar
 * dominios -- p.ej. pasar el @c now del allocator (que vive en MachineIR) a un
 * analisis del IR como @c UseDefFacts.  Mueve la regla "no cruzar niveles" del
 * IR al MachineIR de una REVISION HUMANA a una PROPIEDAD DEL SISTEMA DE TIPOS
 * (igual que
 * @c LaneHazard hizo con las restricciones de lane).  El almacenamiento interno
 * de los Facts sigue siendo @c uint32_t crudo; el tipo fuerte vive en la
 * INTERFAZ.
 *
 * REGLA GENERAL: un Fact PERTENECE a un dominio -- su ValueId y su Position son
 * de ese nivel.  El mismo concepto puede existir en varios (p.ej. el next-use:
 * @c analysis::UseDefFacts en IR, @c jit::MachineNextUseFacts en MachineIR) sin
 * ser duplicacion.  Cuando lleguen @c CFGPos / @c ProfilePos / ... seran
 * "posiciones" pero ninguna intercambiable -- el error que merece atrapar el
 * compilador.
 *
 * El ESTADO "invalido" (sin proximo uso / sin definir / centinela) se expresa
 * con
 * @c invalid() + @c is_valid(), NO comparando contra un entero magico: una
 * posicion invalida es un ESTADO, no una posicion real.
 */

#ifndef VESTA_IR_LINEAR_POS_H
#define VESTA_IR_LINEAR_POS_H

#include <cassert>
#include <cstdint>

namespace ir {

/**
 * @struct LinearPos
 * @brief Posicion lineal del dominio IR.  Comparable y restable consigo misma;
 *        SIN conversion implicita a @c uint32_t ni a otros dominios.
 */
struct LinearPos {
    uint32_t value = 0;

    constexpr LinearPos() noexcept = default;
    explicit constexpr LinearPos(uint32_t v) noexcept : value(v) {}

    /// Centinela: NO es una posicion real, es el ESTADO "invalida" (sin proximo
    /// uso, sin definir, ...).  Encapsula el valor magico para que el codigo
    /// use
    /// @c is_valid() en vez de comparar contra @c 0xFFFFFFFF.
    static constexpr LinearPos invalid() noexcept {
        return LinearPos{0xFFFFFFFFu};
    }
    /// True si es una posicion REAL (no el centinela @c invalid()).
    constexpr bool is_valid() const noexcept { return value != 0xFFFFFFFFu; }

    constexpr bool operator<(LinearPos o) const noexcept {
        return value < o.value;
    }
    constexpr bool operator<=(LinearPos o) const noexcept {
        return value <= o.value;
    }
    constexpr bool operator>(LinearPos o) const noexcept {
        return value > o.value;
    }
    constexpr bool operator>=(LinearPos o) const noexcept {
        return value >= o.value;
    }
    constexpr bool operator==(LinearPos o) const noexcept {
        return value == o.value;
    }
    constexpr bool operator!=(LinearPos o) const noexcept {
        return value != o.value;
    }

    /// Distancia entre posiciones del MISMO dominio.  PRECONDICION: @c *this >=
    /// @p o (el consumidor lo garantiza); en debug un @c assert caza el uso
    /// incorrecto.
    uint32_t operator-(LinearPos o) const noexcept {
        assert(value >= o.value &&
               "ir::LinearPos::operator-: precondicion this >= o");
        return value - o.value;
    }
};

} // namespace ir

#endif // VESTA_IR_LINEAR_POS_H
