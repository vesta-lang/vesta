/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/frame_emit.cpp
 * @brief Implementacion del prologo y el epilogo.  Ver jit/frame_emit.h.
 */

#include "jit/frame_emit.h"

#include "vesta_rt/abi.h" // VESTA_PROC_STACK_POINTER_OFFSET

namespace jit {

namespace {

/** @brief Operando de registro de 64 bits. */
MOperand r64(MReg r) { return MOperand::make_reg(r, 8); }

/** @brief `push r`. */
MInstr push_of(MReg r) {
    MInstr i;
    i.op = MOp::PUSH;
    i.src1 = r64(r);
    return i;
}

/** @brief `pop r`. */
MInstr pop_of(MReg r) {
    MInstr i;
    i.op = MOp::POP;
    i.dst = r64(r);
    return i;
}

} // namespace

void emit_frame_prologue(const FrameSpec &f, std::vector<MInstr> &out) {
    if (f.naked) return; // el cuerpo es dueno de la pila

    if (f.fpo) {
        /* Se salva RBP porque hay que devolverlo, pero NO se apunta a la pila:
         * asi queda libre para el ABI que lo reclame (el sexto argumento de una
         * llamada al sistema en 32 bits, por ejemplo).  El marco se direcciona
         * por RSP. */
        out.push_back(push_of(MReg::RBP));
    } else if (!f.no_frame) {
        out.push_back(push_of(MReg::RBP));
        out.push_back(MInstr::make_unary(MOp::MOV, r64(MReg::RBP),
                                         r64(MReg::RSP)));
    }

    if (f.bajo_vm) {
        // RBX lleva el ProcessVM* durante toda la funcion; hay que devolverlo.
        out.push_back(push_of(MReg::RBX));
        out.push_back(MInstr::make_unary(MOp::MOV, r64(MReg::RBX),
                                         r64(f.vm.proc_arg)));
    }
    for (uint8_t r : f.callee_saved)
        out.push_back(push_of(static_cast<MReg>(r)));

    if (f.spill_bytes > 0) {
        /* Un marco mayor que una pagina no se puede reservar de un salto: el
         * sistema deja una pagina de GUARDA justo debajo y solo baja ese limite
         * cuando se toca ESA pagina.  Un salto grande la salta, y el primer
         * acceso al marco cae mas alla del limite -- el programa muere en el
         * prologo, antes de ejecutar nada suyo.
         *
         * Por eso se toca una pagina cada vez, de arriba abajo, antes de bajar
         * el puntero: cada toque cae o en memoria ya valida o justo en la
         * guarda, que es lo que la hace bajar.  El tamano se conoce al
         * compilar, asi que los toques van escritos uno a uno: ni bucle ni
         * contador, y cero coste donde el marco no pasa de una pagina.
         *
         * Se escribe el propio puntero de pila porque hace falta ESCRIBIR
         * (leer no siempre basta) y ese hueco es del marco que se reserva. */
        constexpr uint32_t kPagina = 4096;
        for (uint32_t bajada = kPagina; bajada < f.spill_bytes;
             bajada += kPagina) {
            out.push_back(MInstr::make_unary(
                MOp::MOV,
                MOperand::make_mem(MReg::RSP, -static_cast<int32_t>(bajada)),
                r64(MReg::RSP)));
        }
        out.push_back(MInstr::make_unary(MOp::SUB, r64(MReg::RSP),
                                         MOperand::make_imm32(f.spill_bytes)));
    }

    if (f.bajo_vm && f.vm.has_alloca) {
        /* Guardar el RSP de la VM: lo que se reserve de su pila mas adelante se
         * devuelve en el epilogo.  Sin esto, cada llamada se dejaria un trozo. */
        out.push_back(MInstr::make_unary(
            MOp::MOV, r64(f.vm.scratch),
            MOperand::make_mem(MReg::RBX, VESTA_PROC_STACK_POINTER_OFFSET)));
        out.push_back(MInstr::make_unary(
            MOp::MOV, MOperand::make_mem(MReg::RBP, f.vm.rsp_save_off),
            r64(f.vm.scratch)));
    }
}

void emit_frame_epilogue(const FrameSpec &f, std::vector<MInstr> &out) {
    if (f.naked) return; // el cuerpo provee su propio retorno

    if (f.fpo) {
        /* Sin puntero de marco, la reserva se deshace sumando: RSP es estable
         * porque esta forma solo se usa en hojas. */
        if (f.spill_bytes > 0)
            out.push_back(MInstr::make_unary(
                MOp::ADD, r64(MReg::RSP),
                MOperand::make_imm32(f.spill_bytes)));
        for (size_t i = f.callee_saved.size(); i-- > 0;)
            out.push_back(pop_of(static_cast<MReg>(f.callee_saved[i])));
        if (f.bajo_vm) out.push_back(pop_of(MReg::RBX));
        out.push_back(pop_of(MReg::RBP));
        return;
    }

    if (f.no_frame) {
        // No hubo ni RBP ni reserva: solo se deshacen los push.
        for (size_t i = f.callee_saved.size(); i-- > 0;)
            out.push_back(pop_of(static_cast<MReg>(f.callee_saved[i])));
        if (f.bajo_vm) out.push_back(pop_of(MReg::RBX));
        return;
    }

    /* Devolver el RSP de la VM ANTES de desmontar el marco, que es cuando RBP y
     * RBX todavia valen. */
    if (f.bajo_vm && f.vm.has_alloca) {
        out.push_back(MInstr::make_unary(
            MOp::MOV, r64(f.vm.scratch),
            MOperand::make_mem(MReg::RBP, f.vm.rsp_save_off)));
        out.push_back(MInstr::make_unary(
            MOp::MOV,
            MOperand::make_mem(MReg::RBX, VESTA_PROC_STACK_POINTER_OFFSET),
            r64(f.vm.scratch)));
    }
    /* Deshace la reserva y deja RSP en el ultimo registro salvado, de una vez. */
    out.push_back(MInstr::make_unary(
        MOp::LEA, r64(MReg::RSP),
        MOperand::make_mem(
            MReg::RBP, -static_cast<int32_t>(f.slot_size * f.total_saved))));
    for (size_t i = f.callee_saved.size(); i-- > 0;)
        out.push_back(pop_of(static_cast<MReg>(f.callee_saved[i])));
    if (f.bajo_vm) out.push_back(pop_of(MReg::RBX));
    out.push_back(pop_of(MReg::RBP));
}

} // namespace jit
