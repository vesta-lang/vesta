/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/ir_ids.h
 * @brief Como se nombra un bloque o un valor del intermedio DESDE UNA PRUEBA.
 *
 * Las pruebas construyen el intermedio a mano, y ahi los identificadores se
 * escriben como numeros: "el bloque 1", "el valor 4".  @c IrBlockId e
 * @c IrValueId son tipos distintos justo para que ese numero no se cuele solo
 * -- un valor usado como bloque compilaba y funcionaba por coincidencia
 * numerica, y asi aparecieron tres fallos reales que ningun test cazaba --,
 * de modo que construirlos es EXPLICITO.
 *
 * Estas dos funciones son el unico sitio donde ocurre esa conversion en las
 * pruebas.  Lo que se gana no es escribir menos, sino que el sitio de llamada
 * DIGA que es lo que hay: `br(blk(1))` y `operands.push_back(vid(4))` no se
 * pueden confundir, mientras que un ayudante que aceptara el numero pelado
 * devolveria el problema exactamente a la frontera que esto viene a cerrar.
 */

#ifndef VESTA_TESTS_IR_IDS_H
#define VESTA_TESTS_IR_IDS_H

#include "ir/ssa_ir.h"

#include <cstdint>

/// @brief El bloque numero @p n.
static inline constexpr ir::IrBlockId blk(uint32_t n) noexcept {
    return ir::IrBlockId(n);
}

/// @brief El valor numero @p n.
static inline constexpr ir::IrValueId vid(uint32_t n) noexcept {
    return ir::IrValueId(n);
}

#endif // VESTA_TESTS_IR_IDS_H
