/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file concepts.h
 * @brief Conceptos / constraints de genericos (#6).
 *
 * Un concepto es un PREDICADO COMPTIME sobre un tipo (bool).  Se evalua al
 * monomorphizar un generico con bound `<T: Concepto>`; si devuelve false,
 * error claro.  Cero codigo emitido: las constraints desaparecen tras el
 * type-check.  Hay conceptos BUILT-IN (Numeric, Comparable, Sized, ...) y de
 * USUARIO (`concept N<T> = ...;` / `{ ... }` / `{ metodos }`).
 *
 * Modulo separado de type_checker.cpp / comptime_introspect.cpp para
 * mantener cada fichero manejable.
 */

#ifndef VX_CONCEPTS_H
#define VX_CONCEPTS_H

#include "vx/types.h"

#include <string>

namespace vx {

class TypeChecker;

/**
 * @struct ConceptEval
 * @brief Resultado de evaluar un concepto sobre un tipo.
 */
struct ConceptEval {
    bool found = false;     ///< el concepto existe (built-in o de usuario)
    bool satisfied = false; ///< el tipo lo cumple
};

/// @brief ¿@p name es un concepto BUILT-IN del lenguaje?
bool is_builtin_concept(const std::string &name);

/// @brief Evalua el concepto @p name sobre el tipo @p t.
///
/// Resuelve built-in y de usuario (predicado / bloque / estructural).  Los
/// predicados de usuario pueden COMPONER otros conceptos (`Comparable<T>()`)
/// porque la evaluacion pasa por @c comptime_eval_expr, que reconoce los
/// nombres de concepto.  Cota dura de recursion contra conceptos ciclicos.
ConceptEval comptime_eval_concept(const TypeChecker &tc,
                                  const std::string &name, const Type &t);

} // namespace vx

#endif // VX_CONCEPTS_H
