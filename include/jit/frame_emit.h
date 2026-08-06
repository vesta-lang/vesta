/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/frame_emit.h
 * @brief Prologo y epilogo de una funcion: montar y desmontar su marco.
 *
 * No decide NADA.  Que registros hay que salvar, cuanta pila hace falta y si
 * la funcion lleva puntero de marco son cosas que ya vienen decididas -- del
 * asignador y del objetivo -- y aqui solo se traducen a instrucciones.  Por
 * eso recibe un @ref FrameSpec y no la funcion entera: si algo no esta en esa
 * ficha, es que no se necesita para emitir el marco.
 */

#ifndef VESTA_JIT_FRAME_EMIT_H
#define VESTA_JIT_FRAME_EMIT_H

#include "jit/machine_ir.h"

#include <cstdint>
#include <vector>

namespace jit {

/**
 * @struct VmRuntimeFrame
 * @brief Lo que anade el marco cuando el codigo corre BAJO la maquina virtual.
 *
 * Nada de esto existe en un binario nativo: alli no hay runtime debajo.
 */
struct VmRuntimeFrame {
    /// Por donde llega el ProcessVM*, que vive en RBX toda la funcion.
    MReg proc_arg = MReg::RCX;
    /// La funcion reserva pila DE LA VM, que hay que devolver al salir.
    bool has_alloca = false;
    /// Hueco del marco donde se guarda el RSP de la VM mientras tanto.
    int32_t rsp_save_off = 0;
    /// Registro libre para los apanos de guardar y devolver ese RSP.
    MReg scratch = MReg::R10;
};

/**
 * @struct FrameSpec
 * @brief Todo lo que hace falta saber para montar y desmontar un marco.
 */
struct FrameSpec {
    /// Sin prologo ni epilogo: el cuerpo (asm) es dueno de la pila.
    bool naked = false;
    /// Sin puntero de marco: se salva RBP pero no se apunta a la pila, para
    /// dejarlo libre a un ABI que lo use como argumento.  El marco se
    /// direcciona por RSP.
    bool fpo = false;
    /// Hoja sin marco: ni RBP ni reserva de pila.
    bool no_frame = false;
    uint32_t spill_bytes = 0;      ///< bytes de marco a reservar.
    uint32_t slot_size = 8;        ///< tamano de un push (8 en 64, 4 en 32).
    uint32_t total_saved = 0;      ///< cuantos registros se salvan en total.
    std::vector<uint8_t> callee_saved; ///< los que hay que devolver como estaban.

    /// El codigo corre BAJO la maquina virtual (JIT), con su runtime debajo.
    /// Un binario nativo NO lo esta: ahi no hay ProcessVM ni pila de la VM que
    /// devolver, asi que esto se queda a false y @ref vm no se mira.  Va como
    /// una pieza aparte y no como banderas sueltas justamente para que no se
    /// mezclen los dos mundos.
    bool bajo_vm = false;
    VmRuntimeFrame vm;             ///< solo se mira si @ref bajo_vm.
};

/**
 * @brief Emite el prologo de @p f al final de @p out.
 *
 * @param f Ficha del marco.
 * @param out Destino.
 */
void emit_frame_prologue(const FrameSpec &f, std::vector<MInstr> &out);

/**
 * @brief Emite el epilogo de @p f al final de @p out.
 *
 * @param f Ficha del marco.
 * @param out Destino.
 */
void emit_frame_epilogue(const FrameSpec &f, std::vector<MInstr> &out);

} // namespace jit

#endif // VESTA_JIT_FRAME_EMIT_H
