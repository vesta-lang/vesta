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
 * @brief Copiar un MIEMBRO de un tipo (metodo o campo) con los parametros de
 *        tipo sustituidos.
 *
 * Un miembro se copia en cuatro sitios: al instanciar un struct o una clase
 * generica, al aplanar la herencia de un struct y al inyectar lo que trae un
 * concepto.  Cada uno llevaba su propia lista de "que se copia", y las listas
 * divergian: el aplanado perdia `@Override`, la instancia de un struct perdia
 * todo lo de un campo salvo el tipo, el ancho de bits y el valor por defecto
 * (un miembro anonimo, una direccion `in`, un campo `static`...), y ninguna
 * copiaba las propiedades de una clase.  Ahora la lista es UNA.
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
 * Copia TODA la declaracion -- banderas, contratos, propiedades, parametros de
 * tipo del metodo, cotas y procedencia --; lo que cambia segun quien copia (el
 * nombre del constructor de una instancia, las cotas que quedan, los contratos
 * con `when:` que ya se pueden resolver) lo ajusta quien llama.  El hueco del
 * layout no se copia: es de cada tipo.
 *
 * @param m    El metodo original.
 * @param g    La sustitucion.
 * @param body Si se copia el cuerpo.
 * @return La copia.
 */
std::unique_ptr<ast::ClassMethodDecl>
clone_method_with_subst(const ast::ClassMethodDecl &m, const GenSubst &g,
                        MethodBodyCopy body);

/**
 * @brief Copia un campo de struct con los parametros de tipo sustituidos,
 *        con todo lo que lleva (overlay, bits, direccion, valor por defecto,
 *        procedencia).
 * @param f El campo original.
 * @param g La sustitucion.
 * @return La copia.
 */
ast::StructFieldDecl clone_struct_field_with_subst(const ast::StructFieldDecl &f,
                                                   const GenSubst &g);

/**
 * @brief Copia un campo de clase con los parametros de tipo sustituidos.
 * @param f El campo original.
 * @param g La sustitucion.
 * @return La copia.
 */
ast::ClassFieldDecl clone_class_field_with_subst(const ast::ClassFieldDecl &f,
                                                 const GenSubst &g);

} // namespace vxgen
} // namespace vx

#endif // VX_GENERICS_MEMBER_CLONE_H
