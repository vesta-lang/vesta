/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file guarded_call.cpp
 * @brief Implementacion de @ref ir::split_guarded_call.
 */

#include "ir/passes/guarded_call.h"

#include "util/named_alloc.h"

#include <utility>
#include <vector>

namespace ir {

namespace scratch {
struct GuardBlocks; ///< Bloques de guarda de un sitio.
struct FastBlocks;  ///< Bloques del camino directo de un sitio.
struct FastResults; ///< Resultado de cada camino directo.
} // namespace scratch

/**
 * @brief Anade a @p fn.blocks[@p from] un salto incondicional a @p to.
 * @param fn   Funcion.
 * @param from Bloque que salta.
 * @param to   Destino.
 * @param line Linea de fuente.
 */
static void append_br(IrFunction &fn, IrBlockId from, IrBlockId to,
                      uint32_t line) {
    IrInstr br;
    br.op = IrOp::BR;
    br.target_block = to;
    br.source_line = line;
    fn.blocks[from].instrs.push_back(std::move(br));
}

void split_guarded_call(IrFunction &fn, IrBlockId block, size_t pos,
                        const IrOperands &fast_operands, const GuardedArm *arms,
                        size_t narms) {
    if (narms == 0 || block >= fn.blocks.size() ||
        pos >= fn.blocks[block].instrs.size())
        return;
    /* Copias: la cirugia mueve y trunca el bloque. */
    const IrInstr original = fn.blocks[block].instrs[pos];
    const IrValueId dst = original.dst;
    const IrType rtype = original.type;
    const uint32_t line = original.source_line;
    const std::vector<IrBlockId> orig_succs = fn.blocks[block].succs;

    /* Bloques nuevos, al final: los existentes no se mueven.  Se accede
     * siempre por indice, que sobrevive a que el vector crezca. */
    fn.blocks.reserve(fn.blocks.size() + 2 * narms + 2);
    util::NamedVector<IrBlockId, scratch::FastBlocks> fast(narms);
    util::NamedVector<IrBlockId, scratch::GuardBlocks> guard(narms, block);
    for (size_t n = 0; n < narms; ++n)
        fast[n] = fn.new_block("guard_fast");
    for (size_t n = 1; n < narms; ++n)
        guard[n] = fn.new_block("guard_next");
    const IrBlockId fallback = fn.new_block("guard_fallback");
    const IrBlockId merge = fn.new_block("guard_merge");

    /* Lo que iba detras de la llamada pasa a la union; el bloque se queda con
     * lo de delante. */
    {
        std::vector<IrInstr> &b = fn.blocks[block].instrs;
        fn.blocks[merge].instrs.assign(
            std::make_move_iterator(b.begin() + static_cast<long>(pos) + 1),
            std::make_move_iterator(b.end()));
        b.resize(pos);
    }

    util::NamedVector<IrValueId, scratch::FastResults> results(narms,
                                                               IR_NO_VALUE);
    for (size_t n = 0; n < narms; ++n) {
        const IrBlockId g = guard[n];
        const IrBlockId next = n + 1 < narms ? guard[n + 1] : fallback;
        IrInstr br;
        br.op = IrOp::BR_COND;
        br.operands.push_back(arms[n].cond);
        br.target_block = fast[n];
        br.false_block = next;
        br.source_line = line;
        fn.blocks[g].instrs.push_back(std::move(br));
        fn.blocks[g].succs = {fast[n], next};
        if (n > 0) fn.blocks[g].preds = {guard[n - 1]};

        IrInstr call;
        call.op = IrOp::CALL;
        call.type = rtype;
        if (dst != IR_NO_VALUE) {
            results[n] = fn.new_value(rtype);
            fn.values[results[n]].same_pointer_as(fn.values[dst]);
        }
        call.dst = results[n];
        call.func_name = arms[n].callee.str();
        call.operands = fast_operands;
        call.source_line = line;
        call.source_column = original.source_column;
        call.inline_site = original.inline_site;
        fn.blocks[fast[n]].instrs.push_back(std::move(call));
        append_br(fn, fast[n], merge, line);
        fn.blocks[fast[n]].preds = {g};
        fn.blocks[fast[n]].succs = {merge};
    }

    /* Respaldo: la llamada original, tal cual. */
    IrValueId slow = IR_NO_VALUE;
    {
        IrInstr copy = original;
        if (dst != IR_NO_VALUE) {
            slow = fn.new_value(rtype);
            fn.values[slow].same_pointer_as(fn.values[dst]);
        }
        copy.dst = slow;
        fn.blocks[fallback].instrs.push_back(std::move(copy));
    }
    append_br(fn, fallback, merge, line);
    fn.blocks[fallback].preds = {guard[narms - 1]};
    fn.blocks[fallback].succs = {merge};

    /* Union: el resultado de quien haya ido. */
    if (dst != IR_NO_VALUE) {
        IrInstr phi;
        phi.op = IrOp::PHI;
        phi.type = rtype;
        phi.dst = dst;
        for (size_t n = 0; n < narms; ++n)
            phi.phi_args.push_back(IrPhiArg{results[n], fast[n]});
        phi.phi_args.push_back(IrPhiArg{slow, fallback});
        phi.source_line = line;
        fn.blocks[merge].instrs.insert(fn.blocks[merge].instrs.begin(),
                                       std::move(phi));
    }
    std::vector<IrBlockId> merge_preds(fast.begin(), fast.end());
    merge_preds.push_back(fallback);
    fn.blocks[merge].preds = std::move(merge_preds);
    fn.blocks[merge].succs = orig_succs;

    /* Los sucesores del bloque partido ahora vienen de la union, y sus PHI
     * tambien. */
    for (IrBlockId s : orig_succs) {
        if (s == IR_NO_BLOCK || s >= fn.blocks.size()) continue;
        IrBlock &sb = fn.blocks[s];
        for (IrBlockId &p : sb.preds)
            if (p == block) p = merge;
        for (IrInstr &in : sb.instrs) {
            if (in.op != IrOp::PHI) continue;
            for (IrPhiArg &pa : in.phi_args)
                if (pa.block == block) pa.block = merge;
        }
    }
    fn.blocks[block].succs = {fast[0], narms > 1 ? guard[1] : fallback};
}

} // namespace ir
