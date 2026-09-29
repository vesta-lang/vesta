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
 * @file member_clone.h
 * @brief Copiar un METODO de un tipo con los parametros de tipo sustituidos,
 *        con o sin su cuerpo.
 *
 * Un metodo se copia al instanciar un struct o una clase generica, al aplanar
 * la herencia de un struct y al inyectar lo que trae un concepto.  Que se
 * copia lo dice la lista de campos del metodo (`vx/ast/fields_decls.h`) y lo
 * hace @ref copy_parsed, como para cualquier otro nodo; lo unico propio de un
 * metodo es que el CUERPO puede quedarse fuera (ver @ref MethodBodyCopy).  Un
 * campo no tiene nada propio: se copia con @ref parsed_copy.
 */

#ifndef VX_GENERICS_MEMBER_CLONE_H
#define VX_GENERICS_MEMBER_CLONE_H

#include "vx/ast.h"
#include "vx/generics/generic_clone.h"

#include <cstdint>
#include <memory>

namespace vx {
namespace vxgen {

/**
 * @enum MethodBodyCopy
 * @brief Si la copia de un metodo se lleva tambien su cuerpo.
 *
 * Una instancia generica solo clona el cuerpo si le toca a este modulo
 * emitirlo (ver el reparto de instancias); el resto de la ficha se copia
 * siempre.
 */
enum class MethodBodyCopy : uint8_t {
    Clone, ///< con el cuerpo, sustituido
    Skip,  ///< solo la declaracion
};

/**
 * @brief Copia un metodo con los parametros de tipo sustituidos.
 *
 * Copia TODA la declaracion escrita -- banderas, contratos, propiedades,
 * parametros de tipo del metodo, cotas y procedencia --; lo que cambia segun
 * quien copia (el nombre del constructor de una instancia, las cotas que
 * quedan, los contratos con `when:` que ya se pueden resolver) lo ajusta quien
 * llama.  El hueco del layout no se copia: es de cada tipo.
 *
 * @param m    El metodo original.
 * @param g    La sustitucion.
 * @param body Si se copia el cuerpo.
 * @return La copia.
 */
std::unique_ptr<ast::ClassMethodDecl>
clone_method_with_subst(const ast::ClassMethodDecl &m, const GenSubst &g,
                        MethodBodyCopy body);

} // namespace vxgen
} // namespace vx

#endif // VX_GENERICS_MEMBER_CLONE_H
