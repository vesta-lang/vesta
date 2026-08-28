/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/lowering_internal.h
 * @brief Lo que las unidades del lowering comparten entre si, y solo entre si.
 *
 * El lowering vivia en UN fichero de mas de cuarenta mil lineas.  Al repartirlo
 * en varias unidades, los helpers que estaban marcados @c static -- visibles
 * solo dentro de aquel fichero -- dejan de alcanzarse desde las demas.  Este
 * cabecero es donde se declaran esos, y NADA MAS: no es la interfaz del
 * lowering (esa es @c vx/lowering.h, la clase), es su cocina.
 *
 * Regla para lo que entra aqui: si algo lo necesita alguien de FUERA del
 * lowering, no es de aqui -- que suba a la interfaz publica.  Si solo lo
 * necesita una unidad, tampoco: que se quede @c static en la suya.
 */

#ifndef VESTA_VX_LOWERING_INTERNAL_H
#define VESTA_VX_LOWERING_INTERNAL_H

#include "ir/ssa_ir.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vx {

/**
 * @brief Deja el nombre de una clase en los datos del modulo y devuelve su
 *        direccion, reutilizandola si ya estaba.
 *
 * La reflexion por nombre (@c forName, @c getClass) necesita el nombre como
 * bytes en memoria del programa, no como cadena del compilador.
 */
uint64_t intern_class_name(ir::IrModule &mod, const std::string &name);

/**
 * @brief Ensambla un bloque @c asm a bytes.
 *
 * Vive con el resto del mini-ensamblador (@c lowering/asm.cpp) y es el unico de
 * su familia que sale de ahi: los bloques @c asm a nivel de DATOS los ensambla
 * @c Lowering::run al montar el modulo, no @c lower_asm.
 *
 * @p sym_refs (opcional) recibe las referencias a simbolos externos -- un
 * @c call o @c jmp a una funcion de Vesta -- para que el driver las resuelva
 * despues; un salto a una etiqueta local lo cierra el propio ensamblador.
 * @return @c false y deja el motivo en @p err si el bloque no ensambla.
 */
bool asmblk_assemble(
    const std::string &body, uint8_t bits, std::vector<uint8_t> &out,
    std::string &err,
    std::vector<ir::IrModule::StaticDataMeta::SymRef> *sym_refs = nullptr);

} // namespace vx

#endif // VESTA_VX_LOWERING_INTERNAL_H
