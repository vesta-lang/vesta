/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/shape_constraints.h
 * @brief Restricciones que impone la FORMA de las instrucciones, para el
 *        asignador.
 *
 * Hay cosas que las vidas de los valores permiten y la instruccion no.  En una
 * operacion de dos operandos no conmutativa -- `dst = dst OP src`, que es como
 * son casi todas en x86 -- el destino se PISA con el primer operando antes de
 * aplicar la operacion.  Si al segundo operando le toco la misma ranura que al
 * destino, hay que salvarlo antes: dos movimientos y un registro temporal.
 *
 * Nada de eso hace falta si el asignador simplemente no los junta, y para eso
 * tiene que saberlo.  Aqui se le dice.
 *
 * El sitio correcto es este y no el reescritor: el reescritor va DESPUES de
 * asignar y lo unico que puede hacer es apanar el estorbo; el asignador puede
 * no crearlo.
 */

#ifndef VESTA_JIT_SHAPE_CONSTRAINTS_H
#define VESTA_JIT_SHAPE_CONSTRAINTS_H

#include "codegen/rbank/constraints.h"
#include "jit/machine_ir.h"

#include <algorithm>
#include <vector>

namespace jit {

/**
 * @brief ¿Es @p op una operacion de dos operandos NO conmutativa?
 *
 * Las conmutativas no necesitan la restriccion: si el segundo operando cae en
 * la ranura del destino, se aplica la operacion al reves y sale lo mismo.
 *
 * @param op Operacion.
 * @return true si el orden de los operandos importa.
 */
inline bool es_dos_operandos_no_conmutativa(MOp op) noexcept {
    switch (op) {
    case MOp::SUBSD:
    case MOp::SUBSS:
    case MOp::DIVSD:
    case MOp::DIVSS: return true;
    default: return false;
    }
}

/**
 * @brief Recoge las restricciones de forma de @p mf.
 *
 * @param mf Funcion en representacion de maquina, antes de asignar.
 * @return El conjunto, vacio si la funcion no tiene ninguna.
 */
inline codegen::rbank::ConstraintSet
recoger_restricciones_de_forma(const MFunction &mf) {
    codegen::rbank::ConstraintSet cs;
    for (const auto &b : mf.blocks) {
        for (const MInstr &in : b.instrs) {
            if (es_dos_operandos_no_conmutativa(in.op) && in.dst.is_vreg() &&
                in.src2.is_vreg()) {
                cs.different_lane(in.dst.vreg_id(), in.src2.vreg_id());
                continue;
            }
            /* Los operandos de un bloque asm estan vivos TODOS a la vez: el
             * cuerpo los nombra por separado y espera registros distintos.
             * Hoy eso se cumple de rebote -- sus intervalos se solapan porque
             * el bloque los lee y los escribe en el mismo punto -- pero no
             * porque nadie lo haya dicho.  Decirlo es barato y evita que un
             * cambio en como se construyen esos intervalos vuelva a juntarlos:
             * ese fallo ya costo caro una vez, y se manifestaba como un bloque
             * usando dos veces el mismo registro. */
            if (in.op != MOp::INLINE_ASM_RAW) continue;
            const uint32_t idx = static_cast<uint32_t>(in.src1.value);
            if (idx >= mf.asm_blobs.size()) continue;
            const AsmBlob &blob = mf.asm_blobs[idx];
            std::vector<uint32_t> vregs = blob.in_vregs;
            for (uint32_t v : blob.out_vregs)
                if (std::find(vregs.begin(), vregs.end(), v) == vregs.end())
                    vregs.push_back(v);
            for (size_t i = 0; i < vregs.size(); ++i)
                for (size_t j = i + 1; j < vregs.size(); ++j)
                    cs.interfere(vregs[i], vregs[j]);
        }
    }
    return cs;
}

} // namespace jit

#endif // VESTA_JIT_SHAPE_CONSTRAINTS_H
