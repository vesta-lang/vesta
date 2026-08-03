/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ir_facts.h
 * @brief Hechos sobre el programa que consultan los reconocedores de patrones.
 *
 * Un reconocedor de patrones no puede exigir que el codigo llegue escrito de
 * una forma exacta: el mismo dato aparece bajo valores SSA distintos segun como
 * se escribio el fuente y por que transformaciones haya pasado.  Sin poder
 * preguntar "estos dos valores son el mismo dato", cada patron acaba atado a
 * una forma concreta y deja de reconocer variaciones que no cambian nada.
 *
 * Ocurrio con el patron del acarreo: exigia que la comparacion usara
 * LITERALMENTE el mismo valor SSA que el sumando, asi que leer un campo dos
 * veces en lugar de guardarlo en una variable ya lo desactivaba.  Al poder
 * preguntar por equivalencia paso de reconocer 1 caso a reconocer 7.
 *
 * Estos son los HECHOS del programa, la primera de las cuatro categorias con
 * las que se construye una decision (hechos, capacidades del objetivo,
 * restricciones duras y funcion objetivo).  Son inmutables: describen lo que el
 * programa ES, no lo que conviene hacer con el.
 *
 * Todas las respuestas son CONSERVADORAS: ante cualquier duda, la respuesta es
 * que no se puede afirmar.
 */

#ifndef VESTA_IR_IR_FACTS_H
#define VESTA_IR_IR_FACTS_H

#include "ir/ssa_ir.h"

namespace ir {

/**
 * @brief Instruccion que define un valor.
 *
 * @param fn Funcion donde buscar.
 * @param v Valor SSA.
 * @return La instruccion que lo produce, o nullptr si no se encuentra.
 */
const IrInstr *ir_def_of(const IrFunction &fn, IrValueId v);

/**
 * @brief Salta las operaciones que solo re-etiquetan un valor sin cambiarlo.
 *
 * Un @c bitcast entre tipos del mismo ancho o una copia no alteran el dato:
 * seguirlos hasta el origen evita que un patron falle por una diferencia que
 * no existe.
 *
 * @param fn Funcion donde buscar.
 * @param v Valor de partida.
 * @return El valor original detras de las copias.
 */
IrValueId ir_strip_copies(const IrFunction &fn, IrValueId v);

/**
 * @brief True si se puede afirmar que dos valores son el mismo dato.
 *
 * Cubre el caso trivial (el mismo valor SSA, salvando copias) y dos LECTURAS de
 * la misma direccion sin ninguna escritura entre ellas -- el fuente lee un
 * campo dos veces y el resultado son dos valores distintos que valen lo mismo.
 *
 * @param fn Funcion a la que pertenecen los valores.
 * @param x Primer valor.
 * @param y Segundo valor.
 * @return true si con seguridad son el mismo dato; false si no, o si no se
 *         puede asegurar.
 */
bool ir_same_value(const IrFunction &fn, IrValueId x, IrValueId y);

} // namespace ir

#endif // VESTA_IR_IR_FACTS_H
