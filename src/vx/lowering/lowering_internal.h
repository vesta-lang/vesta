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
#include "vx/ast.h"

#include <cstdint>
#include <string>
#include <set>
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

/**
 * @brief Direccion de un campo: @p base mas el desplazamiento, como valor.
 *
 * Lo usan tanto la bajada de la POO como la de expresiones, asi que no puede
 * quedarse dentro de ninguna de las dos.
 */
ir::IrValueId emit_field_addr(ir::IrFunction *fn, ir::IrBlockId block,
                              ir::IrValueId base, uint32_t offset,
                              uint32_t line);

/**
 * @brief Reserva el hueco donde se recuerda la clase ya resuelta por su nombre.
 *
 * Ocho ceros -- el hueco -- seguidos de un centinela y del nombre.  El
 * centinela es lo que lo distingue de @c intern_class_name, que interna el
 * mismo nombre para otra cosa: sin el, las dos entradas se fundirian en una.
 */
uint64_t intern_class_cache_slot(ir::IrModule &mod, const std::string &name);

/**
 * @brief Reparte la funcion de arranque del modulo en tandas mas pequenas.
 *
 * @c __module_init acababa siendo una funcion enorme -- una tirada por clase y
 * por aspecto --, y se puede partir porque sus bloques forman una cadena lineal
 * que NO se pasa valores: lo que una clase necesita de otra viaja por estado
 * global.  Es conservador a proposito: cualquier forma que no encaje -- un PHI,
 * una rama, un valor que cruce y no sea una reserva de pila -- deja la funcion
 * como estaba, porque partir mal aqui no da un programa mas lento, da uno que
 * registra mal sus clases.
 *
 * @param init La funcion recien generada; queda reescrita a llamadas.
 * @param out  Modulo donde se anaden las tandas.
 * @return @c true si se partio; @c false si se dejo intacta.
 */
bool split_module_init_into_chunks(ir::IrFunction &init, ir::IrModule &out);

/**
 * @brief Recoge los nombres a los que un sub-arbol ASIGNA.
 *
 * Lo necesita quien construye un bucle: una variable que el cuerpo modifica
 * tiene que entrar por una phi en la cabecera, o el bucle leeria siempre el
 * valor de la primera vuelta.  Es deliberadamente generoso -- puede nombrar
 * cosas que no son del ambito --; filtrar es cosa de quien pregunta, que si
 * sabe que hay declarado justo antes del bucle.
 *
 * @param n   Nodo a inspeccionar; @c nullptr no aporta nada.
 * @param out Conjunto al que se anaden los nombres.
 */
void collect_assigned_vars(const ast::Node *n, std::set<std::string> &out);

} // namespace vx

#endif // VESTA_VX_LOWERING_INTERNAL_H
