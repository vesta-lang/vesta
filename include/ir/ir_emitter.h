/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ir_emitter.h
 * @brief Emisor IR -> texto .vel para VestaVM (lowering).
 *
 * Traduce un IrModule (SSA IR) a texto ensamblador .vel que puede
 * compilarse directamente con el pipeline existente:
 *
 *   IrModule -> ir_emit_module() -> texto .vel
 *            -> vm --build  -> archivo .velb
 *            -> vm --run    -> ejecucion en la VM
 *
 * El emisor aplica primero el optimizador IR segun el nivel indicado,
 * calcula los intervalos de vida y asigna registros VM mediante linear
 * scan antes de generar el codigo ensamblador.
 *
 * Convencion de llamada emitida:
 *   - Argumentos: r1, r2, ... r12 (hasta 12 argumentos)
 *   - Contador de argumentos: r15 (argc)
 *   - Valor de retorno: r0
 *   - Frame: enter/leave (guarda rbp, reserva slots de pila para spill)
 *   - r14: scratch del emisor (no asignado por el allocator)
 *
 * Instrucciones de dos direcciones:
 *   .vel usa formato "op dst, src" donde dst tambien es fuente (dst op= src).
 *   El emisor emite un MOV previo si dst != src del operando izquierdo.
 *
 * Spilling:
 *   Los valores que no caben en r0-r13 se derraman (spill) en la pila local.
 *   El emisor usa "push" al definir y "pop" al recargar cada valor derramado.
 *   Los slots de pila se reservan con "enter N" (N = spill_count).
 */

#ifndef IR_EMITTER_H
#define IR_EMITTER_H

#include "ir/ssa_ir.h"
#include "ir/ir_optimizer.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace ir {

/**
 * @brief Opciones de emision del emisor IR -> .vel.
 */
struct EmitOptions {
    OptLevel opt_level =
        OptLevel::O1; ///< nivel de optimizacion aplicado antes de emitir
    bool emit_comments = true; ///< emitir comentarios con info de origen IR
    bool emit_debug =
        false; ///< emitir comentarios @line N para cada instruccion
    /// Emitir marcadores `// @sm <hex>` en cada safepoint para embeber los
    /// stackmaps precisos de raices GC en el .velb (seccion VSMP).  Siempre
    /// activo: el GC preciso es el comportamiento correcto, no una opcion.
    bool emit_stackmaps = true;
    bool export_all = false; ///< exportar todas las funciones con @Export
    std::string module_name; ///< nombre @Module (vacio = usar mod.name)
    /// CTPE (opt-in): si != nullptr, TRAS optimizar se ejecutan los candidatos
    /// de precomputo (fn evaluable zero-param) en este @c vx::ComptimeRuntime
    /// (opaco aqui) y se inyecta el resultado escalar como CONST.  nullptr = sin
    /// CTPE (comportamiento normal, cero coste).
    void *ctpe_runtime = nullptr;
    /**
     * @brief El modulo que se entrega YA viene optimizado.
     *
     * El emisor trabaja sobre una copia y la optimiza antes de emitir, lo cual
     * es correcto cuando le llega el IR crudo.  Pero el compilador ya optimiza
     * el modulo por su cuenta -- lo necesita para la seccion intermedia y para
     * los comprobadores --, asi que el mismo trabajo se hacia dos veces:
     * medido en un fuente de 5.700 lineas, 52 ms de los 185 que costaba el
     * frontend entero.
     *
     * No se expresa poniendo @c opt_level a @c O0 porque el nivel no significa
     * solo eso: el emisor lo consulta para decidir como emite.  Aqui se dice
     * exactamente lo que se quiere -- no repitas la optimizacion --, sin tocar
     * nada mas.
     */
    bool ya_optimizado = false;
};

/**
 * @brief Resultado de la emision de un modulo IR a texto .vel.
 */
struct EmitResult {
    bool ok;              ///< true si la emision fue exitosa
    std::string vel_text; ///< texto .vel generado (valido si ok)
    std::string error;    ///< mensaje de error si !ok
    /// En que registro fisico dejo el asignador cada valor SSA, por funcion:
    /// @c value_regs[nombre][id] = registro, o 0xFF si el valor no vive en
    /// uno (murio, o se derramo a la pila).
    ///
    /// El asignador es el UNICO que sabe esto y hasta ahora lo tiraba al
    /// terminar, de modo que al explicar un fallo se ensenaba `%8` por un lado
    /// y `r1=0x2a` por otro sin que nada dijera que son la misma cosa.  Quien
    /// orquesta lo estampa en @c IrValue::reg antes de guardar el intermedio,
    /// que es donde tiene sentido: un registro es propiedad del VALOR.
    std::unordered_map<std::string, std::vector<uint8_t>> value_regs;
};

/**
 * @brief Emite un IrModule completo a texto .vel.
 *
 * Pasos internos:
 *   1. ir_optimize(mod, opts.opt_level)
 *   2. Para cada funcion: compute_liveness + allocate_regs
 *   3. Generar prologo de modulo (@Module, @native_lib, @import)
 *   4. Para cada funcion: emitir cuerpo .vel con todos sus bloques
 *
 * @param mod  Modulo IR a emitir (se trabaja sobre una copia interna).
 * @param opts Opciones de emision y nivel de optimizacion.
 * @return Resultado con el texto .vel o el error.
 */
EmitResult ir_emit_module(const IrModule &mod, const EmitOptions &opts = {});

/**
 * @brief Lee un archivo .ir, lo parsea, optimiza y emite texto .vel.
 *
 * Atajo de conveniencia para el flujo completo desde archivo.
 *
 * @param ir_text  Contenido del archivo .ir como cadena.
 * @param opts     Opciones de emision.
 * @return Resultado con el texto .vel o el error.
 */
EmitResult ir_emit_text(const std::string &ir_text,
                        const EmitOptions &opts = {});

} // namespace ir

#endif // IR_EMITTER_H
