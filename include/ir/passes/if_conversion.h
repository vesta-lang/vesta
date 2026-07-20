/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file if_conversion.h
 * @brief Pase de if-conversion: convierte diamantes if/else legales en SELECT.
 *
 * Transforma la seleccion de valor expresada como control de flujo
 *
 *      cond_block:  br.cond %c -> then, else
 *      then:        <ops especulables>; br merge
 *      else:        <ops especulables>; br merge
 *      merge:       %v = phi [%vt, then] [%ve, else]
 *
 * en la forma de expresion pura
 *
 *      cond_block:  <ops de then y else, hoisted>; br merge
 *      merge:       %v = select.T %c, %vt, %ve
 *
 * (y el caso "triangulo" @c if(c){...} sin @c else, donde una rama es el
 * propio @c cond_block).
 *
 * SEPARACION DE RESPONSABILIDADES (importante): este pase decide UNICAMENTE la
 * LEGALIDAD de la transformacion (ambas ramas especulables + un presupuesto de
 * tamano grueso para no crear SELECT de ramas enormes).  NO decide si MERECE LA
 * PENA -- esa es una cuestion de coste/microarquitectura que resuelve un pase
 * posterior cercano al backend (que puede incluso deshacer el SELECT y volver a
 * un salto).  El IR solo EXPRESA que la seleccion existe; asi el mismo SELECT
 * sirve a x86 (cmov), ARM64 (csel), RISC-V, vectorizacion (blend) y al
 * interprete (secuencia sin salto).
 */

#ifndef IR_PASSES_IF_CONVERSION_H
#define IR_PASSES_IF_CONVERSION_H

namespace ir {

struct IrFunction;

/**
 * @brief Convierte los diamantes/triangulos if/else legales de @p fn en SELECT.
 *
 * Solo aplica legalidad (Capa 1: ambas ramas especulables, sin efectos, sin
 * loads/div que puedan atrapar, sin objetos GC en los phi) mas un presupuesto
 * de tamano de rama.  Reescribe el CFG in situ y deja los bloques de rama
 * vacios/inalcanzables (los limpia el pase de bloques inalcanzables).
 *
 * @param fn Funcion SSA a transformar.
 * @return Numero de diamantes/triangulos convertidos a SELECT.
 */
int ir_pass_if_conversion(IrFunction &fn);

} // namespace ir

#endif // IR_PASSES_IF_CONVERSION_H
