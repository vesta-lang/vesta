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
 * @file diagram_labels.h
 * @brief Como se LEE en un diagrama una expresion, un tipo, un valor o una
 *        instruccion del IR.
 *
 * Es la otra mitad del reparto que empezo @c vel_text_model.h: lo que se dice
 * de una cosa no depende del formato en el que se dibuje.  Un `add.i64 %3, %4`
 * se lee igual en DOT que en Mermaid; lo unico propio de cada uno es como se
 * escapa y con que sintaxis se emite el nodo.
 *
 * Estas cuatro estaban escritas dos veces, identicas byte a byte -- ciento
 * cuarenta y cuatro lineas --, y las dos grandes son justo las que mas van a
 * crecer: describir una instruccion del IR cambia cada vez que se anade una
 * operacion, y describir un tipo cada vez que el lenguaje gana una forma.
 * Escritas dos veces, la segunda copia se queda corta el dia que nadie mire.
 */

#ifndef VX_DIAGRAM_LABELS_H
#define VX_DIAGRAM_LABELS_H

#include "ir/ssa_ir.h"
#include "vx/ast.h"

#include <string>
#include <vector>

namespace vx {

/**
 * @brief Texto de una expresion del AST.
 *
 * Sin truncar ni acotar la profundidad: en un diagrama se quiere ver la
 * expresion entera.
 *
 * @param e Expresion, o nullptr.
 * @return Su texto, o "?" si no hay.
 */
std::string fmt_expr(const ast::Expr *e);

/**
 * @brief Texto de una expresion, acotado en profundidad.
 *
 * Cada expresion se reduce a una linea con lo justo para entender el flujo sin
 * abrir el fuente: los operadores mantienen su simbolo, las llamadas muestran
 * nombre y numero de argumentos, y los literales su valor.
 *
 * @param e     Expresion, o nullptr.
 * @param depth Cuantos niveles quedan por bajar.
 * @return Su texto.
 */
std::string fmt_expr_brief(const ast::Expr *e, int depth);

/**
 * @brief Texto de un tipo declarado.
 *
 * @param tn Nodo de tipo, o nullptr.
 * @return Su texto, o "?" si no hay.
 */
std::string fmt_type_helper(const ast::TypeNode *tn);

/**
 * @brief Texto de un tipo declarado, envoltorio de @c fmt_type_helper.
 *
 * @param tn Nodo de tipo, o nullptr.
 * @return Su texto.
 */
std::string fmt_type(const ast::TypeNode *tn);

/**
 * @brief Nombre de un valor SSA: el que le puso quien lo creo, o su numero.
 *
 * @param fn Funcion a la que pertenece.
 * @param id Identificador del valor.
 * @return Su nombre.
 */
std::string fmt_value_id(const ir::IrFunction &fn, ir::IrValueId id);

/**
 * @brief Texto de UNA instruccion del IR, en una linea.
 *
 * Los saltos se escriben con el NOMBRE del bloque destino y no con su numero,
 * que es lo que hace legible un diagrama de flujo.
 *
 * @param fn     Funcion a la que pertenece.
 * @param ins    Instruccion.
 * @param blocks Bloques de la funcion, para poder nombrar los destinos.
 * @return Su texto.
 */
std::string fmt_instr(const ir::IrFunction &fn, const ir::IrInstr &ins,
                      const std::vector<ir::IrBlock> &blocks);

} // namespace vx

#endif // VX_DIAGRAM_LABELS_H
