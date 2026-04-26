/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file liveness.cpp
 * @brief Implementacion del analisis de vivacidad para la SSA IR.
 */

#include "ir/liveness.h"
#include <algorithm>
#include <unordered_map>

namespace ir {

LivenessResult compute_liveness(const IrFunction &fn) {
    LivenessResult result;
    const size_t nblocks = fn.blocks.size();

    result.block_start.resize(nblocks, 0);
    result.block_end.resize(nblocks, 0);
    result.num_instrs = 0;

    if (nblocks == 0) return result;

    // --- Paso 1: asignar posicion lineal a cada instruccion ---
    // Cada bloque ocupa [block_start[b], block_end[b]] inclusive.
    // Un bloque vacio igual ocupa una posicion (para que los predecesores
    // puedan extender intervalos hasta el "final" de ese bloque).
    uint32_t pos = 0;
    for (size_t b = 0; b < nblocks; ++b) {
        result.block_start[b] = pos;
        uint32_t cnt = static_cast<uint32_t>(fn.blocks[b].instrs.size());
        // al menos una posicion por bloque
        uint32_t span = (cnt > 0) ? cnt : 1;
        pos += span;
        result.block_end[b] = pos - 1;
    }
    result.num_instrs = pos;

    const uint32_t UNDEF = result.num_instrs; // sentinel: valor no definido aun

    // --- Paso 2: inicializar un intervalo por valor ---
    // Usamos un mapa temporal; al final lo convertimos a vector.
    std::unordered_map<IrValueId, LiveInterval> imap;
    imap.reserve(fn.values.size());
    for (const auto &v : fn.values) {
        LiveInterval li;
        li.id  = v.id;
        li.def = UNDEF; // no definido aun
        li.end = 0;
        imap[v.id] = li;
    }

    // Los parametros de la funcion estan vivos desde el inicio (posicion 0).
    for (IrValueId pid : fn.params) {
        auto it = imap.find(pid);
        if (it != imap.end()) {
            it->second.def = 0;
            it->second.end = 0; // se actualizara con los usos reales
        }
    }

    // Marca un valor como "usado" en la posicion pos.
    // Actualiza def si aun no tenia definicion (caso raro: uso antes de def en IR invalida).
    auto mark_use = [&](IrValueId vid, uint32_t use_pos) {
        if (vid == IR_NO_VALUE) return;
        auto it = imap.find(vid);
        if (it == imap.end()) return;
        if (it->second.def == UNDEF) it->second.def = use_pos; // no deberia ocurrir en IR valida
        if (use_pos > it->second.end) it->second.end = use_pos;
    };

    // Marca la definicion de un valor en la posicion pos.
    auto mark_def = [&](IrValueId vid, uint32_t def_pos) {
        if (vid == IR_NO_VALUE) return;
        auto it = imap.find(vid);
        if (it == imap.end()) return;
        if (it->second.def == UNDEF) it->second.def = def_pos;
        // en SSA la definicion ocurre exactamente una vez; ignoramos redefiniciones
    };

    // --- Paso 3: recorrer todas las instrucciones ---
    for (size_t b = 0; b < nblocks; ++b) {
        const IrBlock &bb = fn.blocks[b];
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            uint32_t instr_pos = result.block_start[b] + static_cast<uint32_t>(i);
            const IrInstr &ins = bb.instrs[i];

            // Definicion: el valor destino nace aqui
            mark_def(ins.dst, instr_pos);

            // Usos de operandos normales
            for (IrValueId op : ins.operands) {
                mark_use(op, instr_pos);
            }

            // Uso del puntero de funcion en CALLIND
            if (ins.op == IrOp::CALLIND) {
                mark_use(ins.func_ptr, instr_pos);
            }

            // Argumentos phi: el valor V que llega desde el bloque P
            // se considera "usado" al FINAL del bloque P (no en la instruccion phi).
            // Esto modela correctamente que V debe estar en un registro al salir de P.
            for (const auto &pa : ins.phi_args) {
                if (pa.value == IR_NO_VALUE) continue;
                uint32_t pred_end = (pa.block < static_cast<IrBlockId>(nblocks))
                                    ? result.block_end[pa.block]
                                    : instr_pos;
                mark_use(pa.value, pred_end);
            }
        }
    }

    // --- Paso 4: asegurar coherencia en los parametros ---
    // Si un parametro nunca se uso, su end quedaria a 0 < def=0.
    for (IrValueId pid : fn.params) {
        auto it = imap.find(pid);
        if (it != imap.end() && it->second.def == 0 && it->second.end < it->second.def) {
            it->second.end = it->second.def;
        }
    }

    // --- Paso 5: recopilar solo los valores con definicion conocida ---
    result.intervals.reserve(imap.size());
    for (auto &[id, li] : imap) {
        if (li.def < UNDEF) {
            // Garantizar end >= def (un valor usado exactamente en su def tiene end==def)
            if (li.end < li.def) li.end = li.def;
            result.intervals.push_back(li);
        }
    }

    // Ordenar por posicion de definicion (requerido por linear scan)
    std::sort(result.intervals.begin(), result.intervals.end());

    return result;
}

} // namespace ir
