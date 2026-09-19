/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file vx/generics/generic_head.h
 * @brief Repartir los `<...>` de una declaracion: que DECLARA y que PASA.
 *
 * @par La pregunta
 * Las dos cosas se escriben igual:
 *
 * ```vx
 * struct Caja<T>     { ... }   // `T` es una VARIABLE que se declara aqui
 * struct Caja<Punto> { ... }   // `Punto` es un ARGUMENTO: especializacion
 * ```
 *
 * y lo unico que las separa es si ese nombre ya es un tipo.  El parser no
 * puede saberlo: el tipo puede declararse mas abajo, en otro fichero del
 * paquete o llegar importado.
 *
 * @par La regla
 * Los `<...>` de una declaracion **declaran variables**.  Un nombre que ya ES
 * un tipo no puede ser una variable -- seria taparlo, que es malo por si solo
 * --, asi que ahi lo que hay son argumentos y la declaracion es una
 * especializacion.  Todo lo que no sea un identificador desnudo (`i64`, `T*`,
 * `Inner<T>`) es un argumento por su forma, sin necesidad de mirar nada.
 *
 * Asi, de las seis especializaciones que el corpus tenia escritas, cinco se
 * reconocen SOLO por la notacion y la sexta -- `Caja<Punto>` -- por el hecho
 * de que `Punto` esta declarado.
 *
 * @par Por que no lo decide el orden de aparicion
 * La regla anterior era "la primera con ese nombre es la plantilla, las
 * siguientes son especializaciones".  Eso hacia IMPOSIBLE declarar dos
 * plantillas homonimas -- la segunda se leia como especializacion de la
 * primera --, que es justo lo que la sobrecarga de genericas necesita.
 *
 * @par Lo que no se calla
 * Una especializacion sin plantilla primaria es un error, y es lo que salta si
 * alguien llama a una variable de tipo como se llama un tipo ya declarado: la
 * declaracion se lee como especializacion de algo que no existe y se DICE, en
 * vez de cambiar de significado en silencio.  Una cota (`<T: Concepto>`) solo
 * se escribe declarando, asi que ahi el choque se dice directamente.
 *
 * @par Coste
 * Una tabla de nombres por MODULO -- no por declaracion --, construida en una
 * pasada sobre las decls y consultada con vistas a las cadenas del AST, que no
 * copia ninguna.  Clasificar es recorrer los pocos nodos de cada cabeza.
 */

#ifndef VX_GENERICS_GENERIC_HEAD_H
#define VX_GENERICS_GENERIC_HEAD_H

#include "vx/ast.h"

namespace vx {

class TypeChecker;

namespace generics {

/**
 * @brief Reparte los `<...>` sin clasificar de todas las decls del modulo.
 *
 * Al terminar, ninguna queda con @c generic_head_unresolved: cada una tiene
 * @c type_params, @c is_specialization y @c spec_pattern con el significado
 * que el resto del compilador espera.
 *
 * @param tc  Para preguntar si un nombre es un tipo -- incluidos los que
 *            llegan importados, que no estan en @p mod.
 * @param mod El modulo.
 */
void classify_generic_heads(TypeChecker &tc, ast::ModuleNode &mod);

/**
 * @brief El mismo reparto para quien SOLO ha parseado.
 *
 * Se apoya nada mas que en los tipos declarados en @p mod, asi que un
 * `Caja<Punto>` con el `Punto` importado se reparte mal -- ahi no hay forma de
 * saber que es un tipo -- y por eso no da ningun veredicto.  Sirve para lo que
 * no compila: el editor, que necesita saber que nombres son variables de tipo
 * para resaltarlos y para poner la firma en su indice.
 *
 * Existe para que no haya DOS criterios.  Sin ella, quien solo parsea tendria
 * que decidirlo por su cuenta, y el que divergiera seria el del editor, donde
 * nadie mira si acierta.
 *
 * @param mod El modulo.
 */
void classify_generic_heads(ast::ModuleNode &mod);

} // namespace generics
} // namespace vx

#endif // VX_GENERICS_GENERIC_HEAD_H
