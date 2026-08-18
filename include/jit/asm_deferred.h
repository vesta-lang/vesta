/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/asm_deferred.h
 * @brief Ensamblado DIFERIDO de un bloque `asm` con operandos que elige el
 *        compilador.
 *
 * Un bloque `asm ( reg x, ymm v )` no se puede ensamblar mientras se baja el
 * codigo: su cuerpo nombra los operandos con marcadores `$N` y quien pone el
 * registro es el ASIGNADOR, que todavia no ha corrido.  Se aplaza hasta
 * tenerlo, se sustituye cada marcador por su registro y entonces se ensambla.
 *
 * Esta pieza NO decide nada -- de eso se encarga el asignador, que es la unica
 * fuente de verdad sobre que registro va donde.  Aqui solo se traduce: numero
 * de ranura a nombre, y texto a bytes.
 *
 * Cuando algo no cuadra devuelve DATOS -- que fallo y con que valores -- y no
 * una frase: la frase la pone quien informa, que es el unico que sabe en que
 * idioma habla el usuario.  Lo que no se puede hacer es callar: emitir cero
 * bytes en silencio deja un programa que compila, arranca y hace otra cosa.
 */

#ifndef VESTA_JIT_ASM_DEFERRED_H
#define VESTA_JIT_ASM_DEFERRED_H

#include "codegen/allocation_result.h"
#include "jit/machine_ir.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jit {

/**
 * @enum AsmDeferredFallo
 * @brief Por que no se pudo ensamblar un bloque diferido.
 */
enum class AsmDeferredFallo : uint8_t {
    NINGUNO = 0,     ///< salio bien.
    SIN_ENSAMBLADOR, ///< no hay ensamblador registrado.
    SIN_REGISTRO,    ///< a un operando no le toco ranura (banco insuficiente).
    SIN_NOMBRE,      ///< la ranura no se puede nombrar (clase/ancho raros).
    NO_ENSAMBLA,     ///< el ensamblador rechazo el texto ya sustituido.
};

/**
 * @struct AsmDeferredResult
 * @brief Lo que sale de ensamblar un bloque diferido.
 *
 * En el caso malo lleva los DATOS del fallo, no su redaccion: el operando, su
 * clase, su ancho y la ranura.  Quien informa los convierte en un mensaje.
 */
struct AsmDeferredResult {
    bool ok = false;            ///< false si no se pudo; ver @c fallo.
    std::vector<uint8_t> bytes; ///< codigo del bloque (vacio si !ok).
    std::string texto;          ///< el asm con los registros ya puestos.

    AsmDeferredFallo fallo = AsmDeferredFallo::NINGUNO;
    uint32_t operando = 0;   ///< indice $N del operando que fallo.
    uint32_t vreg = 0;       ///< su valor virtual.
    uint8_t clase = 0;       ///< su clase de registro (ASM_RC_*).
    uint16_t ancho = 0;      ///< su ancho en bits.
    int32_t ranura = -1;     ///< la ranura fisica, si llego a haber una.
    bool en_memoria = false; ///< el asignador lo dejo en la pila.
    std::string detalle;     ///< lo que dijo el ensamblador (NO_ENSAMBLA).
};

/**
 * @brief Pone los registros que eligio el asignador y ensambla el bloque.
 *
 * @param b Bloque diferido (plantilla con @c $N + descriptor por operando).
 * @param alloc Resultado del asignador: de donde sale cada ranura.
 * @param vf Funcion virtual, para poder nombrarla en el motivo.
 * @return El codigo, o el motivo por el que no se pudo.
 */
AsmDeferredResult asm_deferred_assemble(const AsmBlob &b,
                                        const codegen::AllocationResult &alloc,
                                        const MFunction &vf);

} // namespace jit

#endif // VESTA_JIT_ASM_DEFERRED_H
