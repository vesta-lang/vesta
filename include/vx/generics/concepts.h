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
#include <vector>

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

/**
 * @brief Los argumentos de un concepto: lo que va DETRAS del tipo comprobado.
 *
 * El primer parametro de un concepto es el tipo que se comprueba; los demas
 * son argumentos.  En `<I: Iterator<i64>>` el contexto pone `I` y esta lista
 * lleva `i64`: se evalua `Iterator<I, i64>`.
 */
using ConceptArgs = std::vector<Type>;

/// @brief Evalua el concepto @p name sobre el tipo @p t.
///
/// Resuelve built-in y de usuario (predicado / bloque / estructural).  Los
/// predicados de usuario pueden COMPONER otros conceptos (`Comparable<T>()`)
/// porque la evaluacion pasa por @c comptime_eval_expr, que reconoce los
/// nombres de concepto.  Cota dura de recursion contra conceptos ciclicos.
///
/// @param tc   Comprobador.
/// @param name Concepto.
/// @param t    Tipo que se comprueba (el primer parametro del concepto).
/// @param args Argumentos del concepto (los demas parametros, en orden).
/// @return Si existe y si se cumple.
ConceptEval comptime_eval_concept(const TypeChecker &tc,
                                  const std::string &name, const Type &t,
                                  const ConceptArgs &args = {});

/**
 * @brief Cuantos argumentos de tipo toma el concepto @p name: el tipo
 *        comprobado mas sus argumentos.  Un builtin toma uno.
 * @param tc   Comprobador.
 * @param name Concepto (con o sin espacio de nombres).
 * @return El numero, o 0 si el concepto no existe.
 */
size_t concept_type_param_count(const TypeChecker &tc,
                                const std::string &name);

namespace ast {
struct CallExpr;
} // namespace ast

/**
 * @brief Evalua la PREGUNTA `Concepto<T, args...>()`: el primer argumento de
 *        tipo es el tipo comprobado y los demas, los argumentos del concepto.
 *
 * Un solo sitio para las dos rutas que la contestan -- al compilar y al
 * bajar --, que antes extraian los argumentos cada una a su manera.
 *
 * @param tc   Comprobador.
 * @param name Concepto.
 * @param call La llamada, con sus argumentos de tipo.
 * @return Si existe y si se cumple; no encontrado si no hay argumento de tipo.
 */
ConceptEval eval_concept_question(const TypeChecker &tc,
                                  const std::string &name,
                                  const ast::CallExpr &call);

} // namespace vx

#endif // VX_CONCEPTS_H
