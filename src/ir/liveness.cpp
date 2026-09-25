/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file liveness.cpp
 * @brief Implementacion del analisis de vivacidad para la SSA IR.
 */

#include "ir/liveness.h"
#include <algorithm>

namespace ir {

LivenessResult compute_liveness(const IrFunction &fn) {
    return compute_liveness(fn, compute_block_liveness(fn));
}

LivenessResult compute_liveness(const IrFunction &fn,
                                const BlockLiveness &live) {
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

    /* --- Paso 2: inicializar un intervalo por valor ---
     *
     * Un VECTOR indexado por el id, no un mapa.  `new_value` da
     * `id = values.size()`, asi que el id de un valor ES su posicion: el mapa
     * que habia aqui era un vector escrito con un hash delante, y cobraba un
     * nodo en el monton por valor -- 882.298 reservas al compilar 441.089
     * lineas -- para acabar volcandose a un vector de todas formas.
     *
     * Y de paso deja de ser una fuente de INDETERMINACION: el volcado del
     * paso 5 recorria el mapa, cuyo orden es arbitrario, y `operator<` solo
     * compara `def`, asi que dos intervalos que empatan salian en un orden
     * distinto en cada compilacion.  Por el vector salen por id, siempre
     * igual. */
    std::vector<LiveInterval> ivs(fn.values.size());
    for (const auto &v : fn.values) {
        if (v.id >= ivs.size()) continue;
        LiveInterval &li = ivs[v.id];
        li.id = v.id;
        li.def = UNDEF; // no definido aun
        li.end = 0;
    }

    // Los parametros de la funcion estan vivos desde el inicio (posicion 0).
    for (IrValueId pid : fn.params) {
        if (pid >= ivs.size()) continue;
        ivs[pid].def = 0;
        ivs[pid].end = 0; // se actualizara con los usos reales
    }

    // Marca un valor como "usado" en la posicion pos.
    //
    // IMPORTANTE: NO tocar def aqui.  En presencia de phi_args con back-edge
    // (loops), el "uso" como argumento phi se procesa cuando se recorre el
    // bloque sucesor (header), pero la definicion del valor esta en el
    // bloque predecesor (body), que aparece DESPUES en el orden lineal.
    // Si seteasemos def=use_pos cuando todavia es UNDEF, machacariamos el
    // def correcto que vendra cuando el liveness procese el body, dejando
    // intervalos colapsados [end,end] que confunden al linear scan y
    // provocan que reuse el mismo registro para valores realmente vivos
    // a la vez (el bug observado del while: sum_next y i_phi compartian r4).
    //
    auto mark_use = [&](IrValueId vid, uint32_t use_pos) {
        if (vid == IR_NO_VALUE || vid >= ivs.size()) return;
        if (use_pos > ivs[vid].end) ivs[vid].end = use_pos;
    };

    // Marca la definicion de un valor en la posicion pos.
    auto mark_def = [&](IrValueId vid, uint32_t def_pos) {
        if (vid == IR_NO_VALUE || vid >= ivs.size()) return;
        if (ivs[vid].def == UNDEF) ivs[vid].def = def_pos;
        // en SSA la definicion ocurre exactamente una vez; ignoramos
        // redefiniciones
    };

    // Permite SOBREESCRIBIR el def con una posicion mas temprana.  Solo
    // usado para destinos PHI: en SSA-on-CFG la copia que materializa
    // el valor se emite al final de cada predecesor (emit_phi_copies),
    // por lo que el destino del PHI vive en su registro asignado desde
    // (al menos) el final del predecesor mas temprano.  Si dejamos el
    // def en la posicion de la instruccion PHI (start del sucesor), el
    // regalloc ve el reg como "libre" durante todos los bloques entre
    // pred_end y phi_instr_pos -- y `live_regs_through_call` decide que
    // no contiene un GC value vivo.  Resultado: una llamada con GC
    // (e.g. strmake_h, __new_X) entre la copia y la instruccion PHI
    // deja el host_ptr stale en el reg, y al cruzar la siguiente
    // iteracion del loop se dereferencia memoria liberada.
    auto mark_def_extend_earlier = [&](IrValueId vid, uint32_t def_pos) {
        if (vid == IR_NO_VALUE || vid >= ivs.size()) return;
        if (ivs[vid].def == UNDEF || def_pos < ivs[vid].def) {
            ivs[vid].def = def_pos;
        }
    };

    // --- Paso 3: recorrer todas las instrucciones ---
    for (size_t b = 0; b < nblocks; ++b) {
        const IrBlock &bb = fn.blocks[b];
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            uint32_t instr_pos =
                result.block_start[b] + static_cast<uint32_t>(i);
            const IrInstr &ins = bb.instrs[i];

            // Definicion: el valor destino nace aqui.  Para PHIs, la
            // sobreescribimos mas abajo con la posicion del predecesor
            // mas temprano (donde realmente se emite la copia).
            mark_def(ins.dst, instr_pos);

            // Usos de operandos normales
            for (IrValueId op : ins.operands) {
                mark_use(op, instr_pos);
            }

            // Uso del puntero de funcion en CALLIND y CALLCLOSURE.  Sin
            // tratar este uso explicitamente, el liveness no extiende el
            // live range del SSA value que produjo fn_addr y el regalloc
            // puede reasignar su registro antes del call -> callvmr a
            // direccion basura.
            if (ins.op == IrOp::CALLIND || ins.op == IrOp::CALLCLOSURE) {
                mark_use(ins.func_ptr, instr_pos);
            }

            // Argumentos phi: el valor V que llega desde el bloque P
            // se considera "usado" al FINAL del bloque P (no en la instruccion
            // phi). Esto modela correctamente que V debe estar en un registro
            // al salir de P.
            //
            // Adicionalmente: el DESTINO del PHI ya esta vivo en su reg
            // desde el final del predecesor (emit_phi_copies emite la
            // copia ahi).  Sin extender el def hacia atras, el regalloc
            // ignora el reg en cualquier llamada entre la copia y la
            // instruccion PHI, y un GC dispara host_ptrs stale en el
            // reg.  Marcamos el def del destino al pred_end mas
            // temprano (over-aproxima por linealidad pero garantiza
            // que `live_regs_through_call` lo vea como vivo).
            uint32_t earliest_pred_end = instr_pos;
            for (const auto &pa : ins.phi_args) {
                if (pa.value == IR_NO_VALUE) continue;
                uint32_t pred_end = (pa.block < static_cast<IrBlockId>(nblocks))
                                        ? result.block_end[pa.block]
                                        : instr_pos;
                mark_use(pa.value, pred_end);
                if (ins.op == IrOp::PHI && pred_end < earliest_pred_end) {
                    earliest_pred_end = pred_end;
                }
            }
            if (ins.op == IrOp::PHI && ins.dst != IR_NO_VALUE &&
                earliest_pred_end < instr_pos) {
                mark_def_extend_earlier(ins.dst, earliest_pred_end);
            }
        }
    }

    // --- Vivos a la salida de cada bloque ---
    //
    // Sin esto, los valores definidos antes de un loop y usados dentro del
    // header se consideran "muertos" tras el ultimo uso lineal en el body, por
    // lo que el regalloc reusa su registro y rompe la siguiente iteracion (bug
    // observado con `i32* p = &x` dentro de un while).
    //
    // Los da `compute_block_liveness`, su unico productor.  Aqui habia un
    // punto fijo propio con un `unordered_set` por bloque, copiado entero en
    // cada vuelta: 7,45 s de 99 medidos con VTune en el `main` de 205.000
    // instrucciones de un fuente generado.
    for (size_t b = 0; b < nblocks; ++b) {
        const uint32_t end_pos = result.block_end[b];
        for (IrValueId v : live.live_out(IrBlockId(b))) {
            if (v >= ivs.size()) continue;
            if (end_pos > ivs[v].end) ivs[v].end = end_pos;
        }
    }

    // Loop carry fix: si un value @c v se USA en una posicion ANTERIOR
    // a su definicion lineal (caso clasico de back-edge: %v def en
    // bloque B6, usado como PHI arg desde bloque B3 que viene antes
    // linealmente), extender el end al final de la funcion para que el
    // linear scan no reuse el reg de @c v en cualquier bloque entre
    // B3 y B6 (que en orden de ejecucion del CFG ESTA en el body del
    // loop, accesible via back-edge).
    //
    // Sin esto, el regalloc reusa el reg de @c v dentro de step/body y
    // los PHI moves al final del back-edge leen basura -> resultados
    // incorrectos en for/while con vars carry-loop.

    // --- Paso 4: asegurar coherencia en los parametros ---
    // Si un parametro nunca se uso, su end quedaria a 0 < def=0.
    for (IrValueId pid : fn.params) {
        if (pid >= ivs.size()) continue;
        if (ivs[pid].def == 0 && ivs[pid].end < ivs[pid].def)
            ivs[pid].end = ivs[pid].def;
    }

    // --- Paso 5: recopilar solo los valores con definicion conocida ---
    result.intervals.reserve(ivs.size());
    for (LiveInterval &li : ivs) {
        if (li.def < UNDEF) {
            // Garantizar end >= def (un valor usado exactamente en su def tiene
            // end==def)
            if (li.end < li.def) li.end = li.def;
            result.intervals.push_back(li);
        }
    }

    // Ordenar por posicion de definicion (requerido por linear scan)
    std::sort(result.intervals.begin(), result.intervals.end());

    return result;
}

} // namespace ir
