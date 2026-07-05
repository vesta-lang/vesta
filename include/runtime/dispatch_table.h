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
 * @file dispatch_table.h
 * @brief Macros de despacho basado en computed-goto para el pipeline de
 * ejecucion.
 *
 * Define los macros DISPATCH() y NEXT() que permiten implementar un despacho
 * de instrucciones de muy alta velocidad usando la extension de computed-goto
 * de GCC/Clang.  La tabla de saltos dispatch_table[2][0x100] mapea el par
 * (is_not_extended, opcode_index) a una etiqueta de codigo.
 *
 * @note La implementacion inline de execute_instr() esta actualmente
 *       comentada; los macros se definen para uso futuro o experimental.
 *
 * @warning Este archivo usa la extension de GCC/Clang de computed-goto
 * (&&label). No es estandar C++17 y genera una advertencia -Wpedantic que se
 *          suprime explicitamente con pragmas de diagnostico.
 */

#ifndef DISPATCH_TABLE_H
#define DISPATCH_TABLE_H

/**
 * @brief Salta a la etiqueta correspondiente al opcode actualmente
 * descodificado.
 *
 * Usa la tabla dispatch_table[is_not_extended][opcode_index] indexada por los
 * campos de la instruccion descodificada activa (decoded_ptr) del proceso.
 * Requiere que la tabla haya sido inicializada previamente.
 */
#define DISPATCH()                                                             \
    goto *dispatch_table[vm->decoded_ptr->is_not_extended]                     \
                        [vm->decoded_ptr->opcode_index]

/**
 * @brief Avanza el PC si no hubo salto, descodifica la siguiente instruccion y
 * despacha.
 *
 * Secuencia: incrementar RIP (si !did_jump) -> decode_instruction() ->
 * DISPATCH(). Usado al final de cada etiqueta de opcode para continuar el bucle
 * de ejecucion sin overhead de llamada a funcion.
 */
#define NEXT()                                                                 \
    if (!decoded_ptr->did_jump) rip.ptr_vm.raw += decoded_ptr->size;           \
    decode_instruction();                                                      \
    DISPATCH()

#include "exec_instruction.h"
#include "runtime.h"

namespace runtime {
class VM; ///< Declaracion adelantada de la instancia VM
}

// Suprimir la advertencia -Wpedantic generada por el uso de computed-goto
// (&&label)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpedantic"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

/*
 * Implementacion experimental de execute_instr() con dispatch de computed-goto.
 * Actualmente comentada; pendiente de integracion con el nuevo modelo de
scheduler.
 *
inline void execute_instr(runtime::VM *vm, const runtime::DecodedInstr &instr) {
    static void *dispatch_table[2][0x100] = {nullptr};
    static bool initialized = false;

    if (!initialized) {
        // primarios
        dispatch_table[0][0x00] = &&OP_NOP;
        // extendidos
        dispatch_table[0][0x05] = &&OP_ADD_reg;

        initialized = true;
    }

    DISPATCH();
OP_NOP:
OP_ADD_reg:
    runtime::exec_instr_add_reg(vm, instr);

OP_INVALID:
    // instruccion invalida
    return;
}
*/

// Restaurar el nivel de advertencias previo
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif // DISPATCH_TABLE_H
