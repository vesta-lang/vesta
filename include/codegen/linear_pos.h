/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/linear_pos.h
 * @brief @c codegen::LinearPos: posicion lineal en el dominio del MachineIR (la
 *        numeracion de @c build_intervals -- 2 posiciones por instruccion: uso en
 *        @c 2*gi (par), def en @c 2*gi+1 (impar)).
 *
 * Vive en @c codegen:: (NO en @c jit::) porque el MachineIR y sus posiciones son del
 * BACKEND GENERAL: los comparten el JIT y el AOT, igual que @c codegen::RegAlloc.  El
 * cambio de casa NO altera la separacion de niveles -- solo nombra correctamente el
 * dominio.
 *
 * TIPO FUERTE, a proposito.  Es un @c uint32_t envuelto que NO se convierte
 * implicitamente a/desde @c uint32_t NI a @c ir::LinearPos (el dominio IR, 1 posicion
 * por instruccion).  El allocator (@c smart_spill) trabaja en ESTE dominio: su @c now y
 * los intervalos de @c build_intervals viven aqui.  Pasar un @c ir::LinearPos a un hecho
 * del MachineIR (o viceversa) NO compila -- la separacion de niveles queda codificada en
 * el sistema de tipos, no solo en la documentacion.  El almacenamiento interno de los
 * Facts es @c uint32_t crudo; el tipo fuerte vive en la INTERFAZ.
 *
 * REGLA GENERAL: un Fact PERTENECE a un dominio -- su ValueId y su Position son de ese
 * nivel.  El mismo concepto puede existir en varios (p.ej. el next-use:
 * @c analysis::UseDefFacts en IR, @c MachineNextUseFacts en MachineIR) sin ser
 * duplicacion.  Cuando lleguen @c CFGPos / @c ProfilePos / ... seran "posiciones" pero
 * ninguna intercambiable -- el error que merece atrapar el compilador.
 *
 * El ESTADO "invalido" (sin proximo uso / centinela) se expresa con @c invalid() +
 * @c is_valid(), NO comparando contra un entero magico: una posicion invalida es un
 * ESTADO, no una posicion real.
 */

#ifndef VESTA_CODEGEN_LINEAR_POS_H
#define VESTA_CODEGEN_LINEAR_POS_H

#include <cassert>
#include <cstdint>

namespace codegen {

/**
 * @struct LinearPos
 * @brief Posicion lineal del dominio MachineIR.  Comparable y restable consigo misma;
 *        SIN conversion implicita a @c uint32_t ni a otros dominios.
 */
struct LinearPos {
    uint32_t value = 0;

    constexpr LinearPos() noexcept = default;
    explicit constexpr LinearPos(uint32_t v) noexcept : value(v) {}

    /// Centinela: NO es una posicion real, es el ESTADO "invalida" (sin proximo uso,
    /// ...).  Encapsula el valor magico para que el codigo use @c is_valid() en vez de
    /// comparar contra @c 0xFFFFFFFF.
    static constexpr LinearPos invalid() noexcept {
        return LinearPos{0xFFFFFFFFu};
    }
    /// True si es una posicion REAL (no el centinela @c invalid()).
    constexpr bool is_valid() const noexcept { return value != 0xFFFFFFFFu; }

    constexpr bool operator<(LinearPos o) const noexcept { return value < o.value; }
    constexpr bool operator<=(LinearPos o) const noexcept { return value <= o.value; }
    constexpr bool operator>(LinearPos o) const noexcept { return value > o.value; }
    constexpr bool operator>=(LinearPos o) const noexcept { return value >= o.value; }
    constexpr bool operator==(LinearPos o) const noexcept { return value == o.value; }
    constexpr bool operator!=(LinearPos o) const noexcept { return value != o.value; }

    /// Distancia entre posiciones del MISMO dominio.  PRECONDICION: @c *this >= @p o
    /// (el consumidor lo garantiza); en debug un @c assert caza el uso incorrecto.
    uint32_t operator-(LinearPos o) const noexcept {
        assert(value >= o.value && "codegen::LinearPos::operator-: precondicion this >= o");
        return value - o.value;
    }
};

/**
 * Codificacion canonica de posiciones del MachineIR: DOS posiciones por instruccion,
 * indexadas por el indice global @c gi de la instruccion.  @c build_intervals la
 * implementa; los CONSUMIDORES (p.ej. el Rewrite) NO deben escribir @c 2*gi a mano --
 * preguntan por el MOMENTO del operando y estas funciones traducen.  Si la codificacion
 * temporal cambia, cambia SOLO aqui y ninguna otra capa se entera: cada capa conoce las
 * preguntas que le pertenecen, no las convenciones internas de otra.
 *
 *   @c use_point(gi): donde se LEEN los operandos fuente de la instruccion @c gi.
 *   @c def_point(gi): donde se DEFINE el resultado de la instruccion @c gi (su intervalo
 *                     empieza aqui -> consultar un dst en use_point daria "no vive aun").
 */
constexpr LinearPos use_point(uint32_t gi) noexcept { return LinearPos{2u * gi}; }
constexpr LinearPos def_point(uint32_t gi) noexcept { return LinearPos{2u * gi + 1u}; }

} // namespace codegen

#endif // VESTA_CODEGEN_LINEAR_POS_H
