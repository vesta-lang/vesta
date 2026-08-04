/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ir_facts.cpp
 * @brief Implementacion de los hechos sobre el programa (ver ir_facts.h).
 */

#include "ir/ir_facts.h"

namespace ir {

/**
 * @brief Instruccion que define un valor, o nullptr si no se encuentra.
 *
 * @param fn Funcion donde buscar.
 * @param v Valor SSA.
 * @return Puntero a la instruccion que lo define.
 */
const IrInstr *ir_def_of(const IrFunction &fn, IrValueId v) {
    if (v == IR_NO_VALUE) return nullptr;
    for (const IrBlock &b : fn.blocks)
        for (const IrInstr &in : b.instrs)
            if (in.dst == v) return &in;
    return nullptr;
}

/**
 * @brief Salta las operaciones que solo re-etiquetan un valor sin cambiarlo.
 *
 * @param fn Funcion donde buscar.
 * @param v Valor de partida.
 * @return El valor original detras de las copias.
 */
IrValueId ir_strip_copies(const IrFunction &fn, IrValueId v) {
    for (int guard = 0; guard < 16; ++guard) {
        const IrInstr *def = ir_def_of(fn, v);
        if (!def || def->operands.empty()) break;
        if (def->op != IrOp::BITCAST && def->op != IrOp::MOV) break;
        v = def->operands[0];
    }
    return v;
}

/**
 * @brief True si @p x e @p y son con seguridad el mismo dato.
 *
 * Ademas del caso trivial (el mismo valor SSA), reconoce dos LECTURAS de la
 * misma direccion sin ninguna escritura entre ellas: el fuente lee un campo
 * dos veces y el resultado son dos valores distintos que valen lo mismo.  Sin
 * esto, un patron deja de reconocerse por algo tan menor como haber escrito
 * `this.lo64` dos veces en lugar de guardarlo en una variable.
 *
 * Es CONSERVADORA: ante cualquier duda responde que no.
 *
 * @param fn Funcion a la que pertenecen los valores.
 * @param x Primer valor.
 * @param y Segundo valor.
 * @return true si se puede afirmar que son el mismo dato.
 */
bool ir_same_value(const IrFunction &fn, IrValueId x, IrValueId y) {
    x = ir_strip_copies(fn, x);
    y = ir_strip_copies(fn, y);
    if (x == y) return x != IR_NO_VALUE;
    const IrInstr *dx = ir_def_of(fn, x);
    const IrInstr *dy = ir_def_of(fn, y);
    if (!dx || !dy) return false;
    if (dx->op != IrOp::LOAD || dy->op != IrOp::LOAD) return false;
    if (dx->operands.empty() || dy->operands.empty()) return false;
    if (dx->type != dy->type) return false;
    // Misma direccion (admitiendo copias) y mismo desplazamiento.
    if (ir_strip_copies(fn, dx->operands[0]) !=
        ir_strip_copies(fn, dy->operands[0]))
        return false;
    if (dx->imm != dy->imm) return false;
    // Y sin ninguna escritura por medio: basta una para que puedan diferir.
    bool visto_x = false, visto_y = false;
    for (const IrBlock &b : fn.blocks) {
        for (const IrInstr &in : b.instrs) {
            const bool es_x = (in.dst == x);
            const bool es_y = (in.dst == y);
            if (es_x) visto_x = true;
            if (es_y) visto_y = true;
            const bool entre = (visto_x != visto_y);
            if (!entre) continue;
            if (in.op == IrOp::STORE || in.op == IrOp::MEMCPY ||
                in.op == IrOp::MEMSET || in.op == IrOp::CALL ||
                in.op == IrOp::CALLVIRT || in.op == IrOp::CALLIND ||
                in.op == IrOp::CALLN)
                return false;
        }
    }
    return visto_x && visto_y;
}

} // namespace ir
