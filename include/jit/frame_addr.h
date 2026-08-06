/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/frame_addr.h
 * @brief Donde vive un valor -> como se nombra.
 *
 * El asignador dice DONDE esta cada valor -- en una ranura del banco o en la
 * pila -- y esto lo traduce a un operando.  No decide nada: si algo no esta en
 * la @ref FrameGeom, es que no hace falta para nombrar una direccion.
 *
 * Va aparte porque la geometria del marco es una sola cosa y hasta ahora estaba
 * repartida: quien calculaba el desplazamiento de una ranura tenia que saber
 * tambien si la funcion usa puntero de marco, cuantos registros se salvaron y
 * de que tamano son.  Cada sitio que lo dedujera por su cuenta era una ocasion
 * de deducirlo distinto.
 */

#ifndef VESTA_JIT_FRAME_ADDR_H
#define VESTA_JIT_FRAME_ADDR_H

#include "codegen/value_location.h"
#include "jit/machine_ir.h"

#include <cstdint>

namespace jit {

/**
 * @struct FrameGeom
 * @brief Geometria del marco: lo justo para situar una ranura de pila.
 */
struct FrameGeom {
    /// Sin puntero de marco: las direcciones van desde RSP y no desde RBP.
    bool fpo = false;
    /// Cuanto hay por debajo cuando se direcciona desde RSP.
    int32_t below = 0;
    /// Tamano de un hueco de pila: 8 en 64 bits, 4 en 32.
    uint32_t slot_size = 8;
    /// Registros salvados en el prologo, que van antes de las ranuras.
    uint32_t total_saved = 0;

    /**
     * @brief Desplazamiento de la ranura @p slot respecto al puntero de marco.
     *
     * Las ranuras van DEBAJO de los registros salvados, de ahi el signo.
     *
     * @param slot Numero de ranura.
     * @return El desplazamiento, negativo.
     */
    int32_t slot_off(uint32_t slot) const noexcept {
        return -static_cast<int32_t>(slot_size * total_saved +
                                     slot_size * (slot + 1u));
    }

    /**
     * @brief Direccion del marco en el desplazamiento @p off.
     *
     * Con puntero de marco es `[rbp + off]`; sin el, `[rsp + below + off]`.
     * Centralizarlo es lo que garantiza que TODO acceso al marco use la misma
     * base -- mezclarlas es escribir en el marco del llamante.
     *
     * @param off Desplazamiento respecto al puntero de marco (negativo).
     * @return El operando de memoria.
     */
    MOperand mem(int32_t off) const noexcept {
        if (fpo) return MOperand::make_mem(MReg::RSP, below + off);
        return MOperand::make_mem(MReg::RBP, off);
    }

    /**
     * @brief Direccion de la ranura @p slot.
     *
     * @param slot Numero de ranura.
     * @return El operando de memoria.
     */
    MOperand slot_mem(uint32_t slot) const noexcept {
        return mem(slot_off(slot));
    }

    /**
     * @brief Tamano del marco en una llamada: la distancia entre los dos
     *        punteros.
     *
     * Lo usa el recolector del binario nativo para reconstruir el puntero de
     * marco desde el del llamante sin seguir la cadena.
     *
     * @param spill_bytes Bytes reservados para ranuras.
     * @return La distancia.
     */
    uint32_t size_for_scan(uint32_t spill_bytes) const noexcept {
        return slot_size * total_saved + spill_bytes;
    }
};

} // namespace jit

#endif // VESTA_JIT_FRAME_ADDR_H
