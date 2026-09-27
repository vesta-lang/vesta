/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file block_order.cpp
 * @brief Implementacion de @ref ir::reorder_blocks_rpo.
 */

#include "ir/passes/block_order.h"

#include "analysis/facts/dom_facts.h"
#include "ir/ssa_ir.h"
#include "util/env_flags.h"

#include <cstdio>
#include <vector>

namespace ir {

void reorder_blocks_rpo(IrFunction &fn) {
    const size_t N = fn.blocks.size();
    if (N <= 1) return;
    /* El RPO es el de los dominadores, sobre el grafo COMPLETO: incluye la
     * arista de cada `tryenter` a su handler.  Sin ella el handler queda
     * inalcanzable en el recorrido y, si se colocara delante, desplazaria al
     * bloque de entrada de la posicion 0 -- se empezaria a ejecutar por el
     * bloque equivocado. */
    const analysis::DomFacts dom = analysis::compute_dom_facts(fn);
    // Nuevo orden fisico: RPO de los ALCANZABLES (deja el entry SIEMPRE en la
    // posicion 0) seguido de los bloques no alcanzables al FINAL.  Ponerlos
    // delante desplazaba el entry y el interprete/emisor arrancaban por el
    // bloque equivocado.
    std::vector<IrBlockId> order;
    order.reserve(N);
    for (IrBlockId b : dom.rpo)
        order.push_back(b);
    size_t unreachable = 0;
    for (size_t b = 0; b < N; ++b)
        if (!dom.reachable(IrBlockId(b))) {
            order.push_back(static_cast<IrBlockId>(b));
            ++unreachable;
        }
    std::vector<IrBlockId> remap(N, IR_NO_BLOCK);
    for (size_t i = 0; i < order.size(); ++i)
        remap[order[i]] = static_cast<IrBlockId>(i);
    static const bool rpo_dump = util::flag_on(util::FlagId::RpoDump);
    if (rpo_dump)
        std::fprintf(stderr, "[rpo] %s: N=%zu inalcanzables=%zu entry->%u\n",
                     fn.name.c_str(), N, unreachable,
                     static_cast<unsigned>(remap[0]));
    bool identity = true;
    for (size_t b = 0; b < N; ++b)
        if (remap[b] != static_cast<IrBlockId>(b)) {
            identity = false;
            break;
        }
    if (identity) return; // ya esta en RPO
    std::vector<IrBlock> nb(N);
    for (size_t b = 0; b < N; ++b) {
        IrBlock bb = std::move(fn.blocks[b]);
        bb.id = remap[b];
        for (auto &p : bb.preds)
            if (p < N) p = remap[p];
        for (auto &s : bb.succs)
            if (s < N) s = remap[s];
        for (auto &ins : bb.instrs) {
            if (ins.target_block != IR_NO_BLOCK && ins.target_block < N)
                ins.target_block = remap[ins.target_block];
            if (ins.false_block != IR_NO_BLOCK && ins.false_block < N)
                ins.false_block = remap[ins.false_block];
            /* Remapear tambien los destinos del SWITCH_DENSE: sin esto, aun
             * con el DFS corregido, los jump_targets[] apuntaban a indices de
             * bloque VIEJOS tras el reorden -> saltos a bloques equivocados. */
            for (auto &jt : ins.jump_targets)
                if (jt != IR_NO_BLOCK && jt < N) jt = remap[jt];
            for (auto &pa : ins.phi_args)
                if (pa.block < N) pa.block = remap[pa.block];
        }
        nb[remap[b]] = std::move(bb);
    }
    fn.blocks = std::move(nb);
}

} // namespace ir
