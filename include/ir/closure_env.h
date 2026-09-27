/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file closure_env.h
 * @brief Por donde llega el entorno de una closure al cuerpo de la lambda, y
 *        lo que un cuerpo lee de los registros de la maquina.
 *
 * Aparte de @c ssa_ir.h porque lo preguntan tambien quienes no necesitan el
 * intermedio entero (los hechos de inline).
 */

#ifndef IR_CLOSURE_ENV_H
#define IR_CLOSURE_ENV_H

#include <cstdint>

namespace ir {

/**
 * @brief El registro de la maquina por el que una @c CALLCLOSURE entrega el
 *        entorno al cuerpo de la lambda, que lo lee con @c READ_VM_REG.
 *
 * Es contrato entre el bajado (que emite la lectura), los backends (que ponen
 * ahi el entorno antes de saltar) y el optimizador (que la sustituye al hacer
 * directa la llamada).  Un numero escrito en cada uno de ellos era tres copias
 * del mismo acuerdo.
 */
inline constexpr uint64_t kClosureEnvVmReg = 14;

/**
 * @enum VmRegReads
 * @brief Que registros de la maquina lee un cuerpo con @c READ_VM_REG.
 *
 * Decide si una funcion se puede llamar directo o copiar en quien la llama:
 * el registro del entorno lo fija una @c CALLCLOSURE, y en ella la lectura se
 * sustituye por su operando; cualquier otro no lo fija ninguna llamada.
 * Ordenado de menos a mas: juntar dos lecturas es quedarse con la mayor.
 */
enum class VmRegReads : uint8_t {
    None,       ///< no lee ningun registro.
    ClosureEnv, ///< solo el del entorno de closure (@ref kClosureEnvVmReg).
    Other,      ///< algun otro, que ninguna llamada fija.
};

/**
 * @brief Junta dos lecturas: manda la que mas restringe.
 * @param a Una.
 * @param b Otra.
 * @return La mayor de las dos.
 */
inline VmRegReads join_vm_reg_reads(VmRegReads a, VmRegReads b) noexcept {
    return a > b ? a : b;
}

} // namespace ir

#endif // IR_CLOSURE_ENV_H
