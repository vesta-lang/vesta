/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file block_liveness.cpp
 * @brief Vivos por bloque en SSA, subiendo desde cada uso hasta su definicion.
 *
 * El porque de no usar un punto fijo esta en la cabecera.  Aqui, el como:
 *
 *   1. Una pasada por las instrucciones apunta, por valor, en que bloques se
 *      define y DESDE DONDE hay que empezar a subir: un uso normal en el
 *      bloque B lo hace vivo a la entrada de B -- salvo que B ya lo haya
 *      definido antes, en cuyo caso el uso no sale del bloque --; un argumento
 *      de phi que llega desde P lo hace vivo a la salida de P.
 *   2. Por cada valor, se sube.  Vivo a la entrada de B -> vivo a la salida de
 *      cada predecesor.  Vivo a la salida de P -> vivo a la entrada de P, salvo
 *      que P lo defina.  Cada bloque se visita UNA vez por valor: la marca que
 *      lo dice es el propio identificador del valor en curso, asi que no hay
 *      que limpiar nada entre un valor y el siguiente.
 *   3. Los pares (bloque, valor) se reparten por bloque con un recuento
 *      ESTABLE; como los valores se recorren en orden, cada fila sale ordenada.
 */

#include "ir/block_liveness.h"

#include <algorithm>

namespace ir {

namespace scratch {
struct LivePredOffsets; ///< Donde empiezan los predecesores de cada bloque.
struct LivePredBlocks;  ///< Los predecesores, bloque tras bloque.
struct LiveDefOffsets;  ///< Donde empiezan los bloques que definen cada valor.
struct LiveDefBlocks;   ///< Los bloques que definen cada valor.
struct LiveDefSeen;     ///< Ultimo bloque en el que se vio definir cada valor.
struct LiveStarts;      ///< Desde donde sube cada valor, sin agrupar.
struct LiveStartOffsets; ///< Donde empiezan los arranques de cada valor.
struct LiveStartsByValue; ///< Los arranques, agrupados por valor.
struct LiveInMark;      ///< Que valor marco por ultimo la entrada del bloque.
struct LiveOutMark;     ///< Que valor marco por ultimo la salida del bloque.
struct LiveWalkStack;   ///< Bloques pendientes de marcar vivos a la entrada.
struct LivePairs;       ///< Pares (bloque, valor) antes de repartirlos.
} // namespace scratch

namespace {

/// Posicion en la lista aplanada de predecesores.
enum PredSlot : uint32_t {};
/// Posicion en la lista aplanada de bloques que definen un valor.
enum DefSlot : uint32_t {};
/// Posicion en la lista aplanada de arranques de un valor.
enum StartSlot : uint32_t {};

/// Por que lado del bloque empieza a vivir un valor.
enum class LiveSide : uint8_t {
    In,  ///< Vivo al ENTRAR: un uso normal que el bloque no define antes.
    Out, ///< Vivo al SALIR: el argumento de un phi de un sucesor.
};

/// Un punto desde el que un valor empieza a subir.
struct LiveStart {
    IrValueId value;
    IrBlockId block;
    LiveSide side;
};

/// Un valor vivo en un lado de un bloque, antes de agrupar por bloque.
struct LivePair {
    IrBlockId block;
    IrValueId value;
};

/**
 * @brief El recorrido hacia arriba de UN valor cada vez.
 *
 * Reune lo que la subida consulta y lo que escribe, para que las dos
 * operaciones -- marcar la entrada, marcar la salida -- no tengan que recibir
 * una docena de argumentos.
 */
struct LiveWalker {
    const util::NamedVector<PredSlot, scratch::LivePredOffsets> &pred_off;
    const util::NamedVector<IrBlockId, scratch::LivePredBlocks> &preds;
    const util::NamedVector<DefSlot, scratch::LiveDefOffsets> &def_off;
    const util::NamedVector<IrBlockId, scratch::LiveDefBlocks> &def_blocks;
    util::NamedVector<IrValueId, scratch::LiveInMark> &in_mark;
    util::NamedVector<IrValueId, scratch::LiveOutMark> &out_mark;
    util::NamedVector<IrBlockId, scratch::LiveWalkStack> &stack;
    util::NamedVector<LivePair, scratch::LivePairs> &in_pairs;
    util::NamedVector<LivePair, scratch::LivePairs> &out_pairs;

    /**
     * @brief Dice si @p b define @p v en alguna de sus instrucciones.
     *
     * En SSA la lista tiene un elemento; se admite mas de uno para que un IR
     * que no lo cumpla de el mismo resultado que el punto fijo, que mata el
     * valor en cualquier bloque que lo defina.
     */
    bool defines(IrBlockId b, IrValueId v) const noexcept {
        for (DefSlot s = def_off[v]; s < def_off[v + 1]; s = DefSlot(s + 1))
            if (def_blocks[s] == b) return true;
        return false;
    }

    /// El valor @p v esta vivo al salir de @p b.
    void mark_out(IrBlockId b, IrValueId v) {
        if (out_mark[b] == v) return; // ya visto para este valor
        out_mark[b] = v;
        out_pairs.push_back(LivePair{b, v});
        if (!defines(b, v)) stack.push_back(b); // sigue vivo a la entrada
    }

    /// Vacia la pila: cada bloque pendiente queda vivo a la entrada.
    void drain(IrValueId v) {
        while (!stack.empty()) {
            const IrBlockId b = stack.back();
            stack.pop_back();
            if (in_mark[b] == v) continue; // ya subido por otro camino
            in_mark[b] = v;
            in_pairs.push_back(LivePair{b, v});
            for (PredSlot s = pred_off[b]; s < pred_off[b + 1];
                 s = PredSlot(s + 1))
                mark_out(preds[s], v);
        }
    }
};

/**
 * @brief Dice si el bloque @p from salta a @p to.
 *
 * Los sucesores de un bloque son uno o dos, asi que recorrerlos es mas barato
 * que cualquier estructura.  Hace falta porque un argumento de phi que nombra
 * un bloque que ya no es predecesor no hace vivo a nadie: el punto fijo solo
 * lo miraba al recorrer los sucesores de ese bloque.
 */
bool jumps_to(const IrFunction &fn, IrBlockId from, IrBlockId to) noexcept {
    for (IrBlockId s : fn.blocks[from].succs)
        if (s == to) return true;
    return false;
}

/**
 * @brief Reparte pares por bloque en forma CSR, conservando su orden.
 * @tparam OffTag Etiqueta de la tabla de desplazamientos.
 * @tparam ValTag Etiqueta de la tabla de valores.
 */
template <typename OffTag, typename ValTag>
void pairs_to_rows(const util::NamedVector<LivePair, scratch::LivePairs> &pairs,
                   uint32_t nb, util::NamedVector<LiveSlot, OffTag> &off,
                   util::NamedVector<IrValueId, ValTag> &vals) {
    off.assign(size_t(nb) + 1, LiveSlot(0));
    for (const LivePair &p : pairs) // cuantos por bloque, un puesto adelantado
        off[p.block + 1] = LiveSlot(off[p.block + 1] + 1);
    for (uint32_t b = 0; b < nb; ++b) // acumulado: inicio de cada fila
        off[b + 1] = LiveSlot(off[b + 1] + off[b]);
    vals.resize(pairs.size());
    util::NamedVector<LiveSlot, OffTag> next(off.begin(), off.end() - 1);
    for (const LivePair &p : pairs) { // estable: el orden de llegada se queda
        vals[next[p.block]] = p.value;
        next[p.block] = LiveSlot(next[p.block] + 1);
    }
}

} // namespace

BlockLiveness compute_block_liveness(const IrFunction &fn) {
    BlockLiveness out;
    const uint32_t nb = static_cast<uint32_t>(fn.blocks.size());
    const uint32_t nv = static_cast<uint32_t>(fn.values.size());
    if (nb == 0) return out;

    /* ---- Predecesores, sacados de los sucesores ----
     * No de `IrBlock::preds`: el punto fijo solo miraba `succs`, y son los que
     * se mantienen al dia en cada transformacion. */
    util::NamedVector<PredSlot, scratch::LivePredOffsets> pred_off(
        size_t(nb) + 1, PredSlot(0));
    for (uint32_t b = 0; b < nb; ++b)
        for (IrBlockId s : fn.blocks[b].succs)
            if (s < nb) pred_off[s + 1] = PredSlot(pred_off[s + 1] + 1);
    for (uint32_t b = 0; b < nb; ++b)
        pred_off[b + 1] = PredSlot(pred_off[b + 1] + pred_off[b]);
    util::NamedVector<IrBlockId, scratch::LivePredBlocks> preds(pred_off[nb]);
    {
        util::NamedVector<PredSlot, scratch::LivePredOffsets> next(
            pred_off.begin(), pred_off.end() - 1);
        for (uint32_t b = 0; b < nb; ++b)
            for (IrBlockId s : fn.blocks[b].succs)
                if (s < nb) {
                    preds[next[s]] = IrBlockId(b);
                    next[s] = PredSlot(next[s] + 1);
                }
    }

    /* ---- Definiciones y arranques, en una pasada ----
     * `seen[v] == b` dice que el bloque en curso YA definio v antes de la
     * instruccion que se mira: un uso asi no sale del bloque.  Los bloques se
     * recorren una vez, asi que la marca no necesita limpiarse. */
    util::NamedVector<DefSlot, scratch::LiveDefOffsets> def_off(size_t(nv) + 1,
                                                                DefSlot(0));
    util::NamedVector<IrBlockId, scratch::LiveDefSeen> seen(nv, IR_NO_BLOCK);
    util::NamedVector<LiveStart, scratch::LiveStarts> starts;
    for (uint32_t b = 0; b < nb; ++b) {
        const IrBlockId bid = IrBlockId(b);
        for (const IrInstr &in : fn.blocks[b].instrs) {
            if (in.op == IrOp::PHI) {
                for (const IrPhiArg &a : in.phi_args)
                    if (a.value < nv && a.block < nb &&
                        jumps_to(fn, a.block, bid))
                        starts.push_back(
                            LiveStart{a.value, a.block, LiveSide::Out});
            } else {
                for (IrValueId u : in.operands)
                    if (u < nv && seen[u] != bid)
                        starts.push_back(LiveStart{u, bid, LiveSide::In});
                if (in.func_ptr < nv && seen[in.func_ptr] != bid)
                    starts.push_back(
                        LiveStart{in.func_ptr, bid, LiveSide::In});
            }
            if (in.dst < nv && seen[in.dst] != bid) { // un bloque, una vez
                seen[in.dst] = bid;
                def_off[in.dst + 1] = DefSlot(def_off[in.dst + 1] + 1);
            }
        }
    }
    for (uint32_t v = 0; v < nv; ++v)
        def_off[v + 1] = DefSlot(def_off[v + 1] + def_off[v]);
    util::NamedVector<IrBlockId, scratch::LiveDefBlocks> def_blocks(
        def_off[nv]);
    {
        util::NamedVector<DefSlot, scratch::LiveDefOffsets> next(
            def_off.begin(), def_off.end() - 1);
        std::fill(seen.begin(), seen.end(), IR_NO_BLOCK);
        for (uint32_t b = 0; b < nb; ++b)
            for (const IrInstr &in : fn.blocks[b].instrs)
                if (in.dst < nv && seen[in.dst] != IrBlockId(b)) {
                    seen[in.dst] = IrBlockId(b);
                    def_blocks[next[in.dst]] = IrBlockId(b);
                    next[in.dst] = DefSlot(next[in.dst] + 1);
                }
    }

    /* ---- Arranques agrupados por valor ----
     * Para subir un valor cada vez, que es lo que deja usar su propio
     * identificador como marca de visitado. */
    util::NamedVector<StartSlot, scratch::LiveStartOffsets> start_off(
        size_t(nv) + 1, StartSlot(0));
    for (const LiveStart &s : starts)
        start_off[s.value + 1] = StartSlot(start_off[s.value + 1] + 1);
    for (uint32_t v = 0; v < nv; ++v)
        start_off[v + 1] = StartSlot(start_off[v + 1] + start_off[v]);
    util::NamedVector<LiveStart, scratch::LiveStartsByValue> by_value(
        starts.size());
    {
        util::NamedVector<StartSlot, scratch::LiveStartOffsets> next(
            start_off.begin(), start_off.end() - 1);
        for (const LiveStart &s : starts) {
            by_value[next[s.value]] = s;
            next[s.value] = StartSlot(next[s.value] + 1);
        }
    }

    /* ---- La subida ---- */
    util::NamedVector<IrValueId, scratch::LiveInMark> in_mark(nb, IR_NO_VALUE);
    util::NamedVector<IrValueId, scratch::LiveOutMark> out_mark(nb,
                                                                IR_NO_VALUE);
    util::NamedVector<IrBlockId, scratch::LiveWalkStack> stack;
    util::NamedVector<LivePair, scratch::LivePairs> in_pairs, out_pairs;
    LiveWalker walk{pred_off, preds,    def_off,  def_blocks, in_mark,
                    out_mark, stack,    in_pairs, out_pairs};
    for (IrValueId v = IrValueId(0); v < nv; ++v) {
        for (StartSlot s = start_off[v]; s < start_off[v + 1];
             s = StartSlot(s + 1)) {
            const LiveStart &st = by_value[s];
            if (st.side == LiveSide::Out)
                walk.mark_out(st.block, v);
            else
                stack.push_back(st.block);
        }
        walk.drain(v);
    }

    pairs_to_rows(in_pairs, nb, out.in_offsets, out.in_values);
    pairs_to_rows(out_pairs, nb, out.out_offsets, out.out_values);
    return out;
}

} // namespace ir
