/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ast_equal.h
 * @brief Si dos arboles dicen LO MISMO que escribio el programador.
 *
 * Compara los campos que escribe el parser (`vx/ast/fields.h`), posiciones
 * incluidas, y no mira los anotados.  Es lo que permite probar que un clon, o
 * lo que vuelva de un `.vxi`, es el original: con el mismo recorrido de campos
 * que el clonador, un campo nuevo que se olvide en la lista falla en los dos
 * sitios a la vez, no en uno solo y en silencio.
 */

#ifndef VX_AST_AST_EQUAL_H
#define VX_AST_AST_EQUAL_H

#include "vx/ast.h"

namespace vx::ast {

/**
 * @brief Si dos expresiones son iguales en todo lo escrito.
 * @param a Una (puede ser nula).
 * @param b Otra (puede ser nula).
 * @return Cierto si las dos son nulas o coinciden campo a campo.
 */
bool same_parsed(const Expr *a, const Expr *b);

/**
 * @brief Si dos sentencias son iguales en todo lo escrito.
 * @param a Una (puede ser nula).
 * @param b Otra (puede ser nula).
 * @return Cierto si las dos son nulas o coinciden campo a campo.
 */
bool same_parsed(const Stmt *a, const Stmt *b);

/**
 * @brief Si dos tipos escritos son iguales en todo lo escrito.
 * @param a Uno (puede ser nulo).
 * @param b Otro (puede ser nulo).
 * @return Cierto si los dos son nulos o coinciden campo a campo.
 */
bool same_parsed(const TypeNode *a, const TypeNode *b);

/**
 * @brief Si dos parametros son iguales en todo lo escrito.
 * @param a Uno.
 * @param b Otro.
 * @return Cierto si coinciden campo a campo.
 */
bool same_parsed(const ParamDecl &a, const ParamDecl &b);

/**
 * @brief Si dos metodos son iguales en todo lo escrito (cuerpo incluido).
 * @param a Uno.
 * @param b Otro.
 * @return Cierto si coinciden campo a campo.
 */
bool same_parsed(const ClassMethodDecl &a, const ClassMethodDecl &b);

/**
 * @brief Si dos entradas de una lista de declaraciones (la de un modulo o un
 *        namespace) son iguales en todo lo escrito: una declaracion con todo
 *        lo que cuelga de ella, o un `comptime { ... }` de nivel superior.
 * @param a Una (puede ser nula).
 * @param b Otra (puede ser nula).
 * @return Cierto si las dos son nulas o coinciden campo a campo.
 */
bool same_parsed_decl(const Node *a, const Node *b);

} // namespace vx::ast

#endif // VX_AST_AST_EQUAL_H
