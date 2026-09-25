/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/facts/dom_facts.cpp
 * @brief Implementacion de @c compute_dom_facts (ver dom_facts.h).
 *
 * Es el codigo que vivia dentro de @c loop_facts.cpp, MOVIDO y no reescrito:
 * grafo desde las aristas de la funcion -> postorden -> dominadores inmediatos
 * (Cooper-Harvey-Kennedy) -> arbol numerado.  Lo unico que cambia es de donde
 * salen los sucesores: de @c IrFunction::edges_of, que es la regla de un solo
 * sitio, en vez de una copia propia que recorria todas las instrucciones.
 */

#include "analysis/facts/dom_facts.h"

#include <cstdint>

namespace analysis {

char DominatorsAnalysis::ID = 0;

using ir::IrBlockId;
using ir::IrFunction;

namespace {

/// Los vecinos de UN bloque mientras se juntan, para quitar repetidos.  Dos
/// en linea, porque eso es lo que suele tener un bloque.
using Neighbors = util::SmallVector<IrBlockId, 2>;
/// Una lista de bloques de trabajo: el postorden, una pila de recorrido.
using BlockList = util::SmallVector<IrBlockId, kDomInlineBlocks>;

/** @brief Anade @p t a @p v si no estaba ya. */
void add_neighbor(Neighbors &v, IrBlockId t) {
    for (IrBlockId x : v)
        if (x == t) return; // sin repetidos
    v.push_back(t);
}

} // namespace

BlockGraph block_successors(const IrFunction &fn, ir::IrEdgeWant want) {
    const size_t N = fn.blocks.size();
    BlockGraph succs;
    succs.offs.assign(N + 1, CfgEdgeSlot(0));
    /* La fila del bloque en curso y la lista de aristas se REUSAN entre
     * bloques: el almacenamiento en linea se paga una vez. */
    Neighbors row;
    ir::IrEdgeList edges;
    for (size_t b = 0; b < N; ++b) {
        row.clear();
        fn.edges_of(IrBlockId(b), want, edges);
        for (const ir::IrEdge &e : edges)
            add_neighbor(row, e.to);
        for (IrBlockId s : row)
            succs.edges.push_back(s);
        succs.offs[b + 1] = CfgEdgeSlot(succs.edges.size());
    }
    return succs;
}

BlockGraph invert_block_graph(const BlockGraph &succs) {
    const size_t N = succs.size();
    BlockGraph preds;
    // Contar cuantos predecesores tiene cada bloque, en offs[b+1]...
    preds.offs.assign(N + 1, CfgEdgeSlot(0));
    for (size_t b = 0; b < N; ++b)
        for (IrBlockId s : succs[b])
            preds.offs[size_t(s) + 1] = CfgEdgeSlot(preds.offs[size_t(s) + 1] + 1);
    // ...y convertir las cuentas en donde empieza cada uno.
    for (size_t b = 0; b < N; ++b)
        preds.offs[b + 1] = CfgEdgeSlot(preds.offs[b + 1] + preds.offs[b]);
    preds.edges.resize(preds.offs[N], IrBlockId(0));
    // Cursor por bloque: donde va el siguiente predecesor suyo.
    util::SmallVector<CfgEdgeSlot, kDomInlineBlocks> next(N, CfgEdgeSlot(0));
    for (size_t b = 0; b < N; ++b)
        next[b] = preds.offs[b];
    for (size_t b = 0; b < N; ++b)
        for (IrBlockId s : succs[b]) {
            preds.edges[next[s]] = IrBlockId(b);
            next[s] = CfgEdgeSlot(next[s] + 1);
        }
    return preds;
}

namespace {

/// Un paso pendiente del recorrido en profundidad: el bloque y por que
/// sucesor suyo va.  Un struct propio y no un `std::pair`, que no es
/// trivialmente copiable y esta tabla se mueve con `memcpy`.
struct DfsStep {
    IrBlockId block;
    uint32_t next;
};

/**
 * @brief Postorden desde @p entry (iterativo: una funcion en linea recta no
 *        desborda la pila nativa).
 * @param po  salida: numero de postorden, o @c NO_POSTORDER si inalcanzable.
 * @param rpo salida: los bloques alcanzables en postorden inverso.
 */
void compute_rpo(const BlockGraph &succs, IrBlockId entry,
                 util::SmallVector<PostorderNum, kDomInlineBlocks> &po,
                 util::SmallVector<IrBlockId, kDomInlineBlocks> &rpo) {
    const size_t N = succs.size();
    po.assign(N, NO_POSTORDER);
    rpo.clear();
    if (size_t(entry) >= N) return;
    util::SmallVector<uint8_t, kDomInlineBlocks> visited(N, 0);
    BlockList order; // postorden
    util::SmallVector<DfsStep, kDomInlineBlocks> stk;
    visited[entry] = 1;
    stk.push_back({entry, 0});
    while (!stk.empty()) {
        DfsStep &top = stk.back();
        if (top.next < succs[top.block].size()) {
            const IrBlockId s = succs[top.block][top.next++];
            if (!visited[s]) {
                visited[s] = 1;
                /* `top` ya no se usa: apilar puede mover la tabla. */
                stk.push_back({s, 0});
            }
        } else {
            order.push_back(top.block);
            stk.pop_back();
        }
    }
    uint32_t n = 0;
    for (IrBlockId b : order)
        po[b] = PostorderNum(n++);
    // RPO = postorden al reves, copiado del final al principio.
    rpo.reserve(order.size());
    for (size_t i = order.size(); i-- > 0;)
        rpo.push_back(order[i]);
}

/**
 * @brief El dominador comun mas cercano de @p a y @p b, subiendo por @p idom.
 *
 * Numero de postorden mas ALTO = mas cerca de la entrada; se sube por el que
 * este mas lejos hasta que se encuentran.
 */
IrBlockId idom_intersect(
    const util::SmallVector<IrBlockId, kDomInlineBlocks> &idom,
    const util::SmallVector<PostorderNum, kDomInlineBlocks> &po, IrBlockId a,
    IrBlockId b) {
    while (a != b) {
        while (po[a] < po[b])
            a = idom[a];
        while (po[b] < po[a])
            b = idom[b];
    }
    return a;
}

/** @brief Dominadores inmediatos por Cooper-Harvey-Kennedy. */
void compute_idom(const BlockGraph &preds,
                  const util::SmallVector<PostorderNum, kDomInlineBlocks> &po,
                  const util::SmallVector<IrBlockId, kDomInlineBlocks> &rpo,
                  IrBlockId entry,
                  util::SmallVector<IrBlockId, kDomInlineBlocks> &idom) {
    const size_t N = preds.size();
    idom.assign(N, ir::IR_NO_BLOCK);
    if (rpo.empty()) return;
    idom[entry] = entry;
    bool changed = true;
    while (changed) {
        changed = false;
        for (IrBlockId b : rpo) {
            if (b == entry) continue;
            IrBlockId new_idom = ir::IR_NO_BLOCK;
            for (IrBlockId p : preds[b]) {
                if (idom[p] == ir::IR_NO_BLOCK) continue; // aun sin procesar
                new_idom = (new_idom == ir::IR_NO_BLOCK)
                               ? p
                               : idom_intersect(idom, po, p, new_idom);
            }
            if (new_idom != ir::IR_NO_BLOCK && idom[b] != new_idom) {
                idom[b] = new_idom;
                changed = true;
            }
        }
    }
}

/// Un paso pendiente del recorrido que numera el arbol.
struct DomWalkStep {
    IrBlockId block;
    DomChildSlot next; ///< siguiente hijo por visitar.
};

/**
 * @brief Los hijos de cada bloque en el arbol, contiguos y en orden de bloque.
 *
 * Se guardan en @p d: hacen falta para numerar el arbol y los recorre quien
 * baja por el (el renombrado de mem2reg).
 */
void build_dom_children(DomFacts &d, IrBlockId entry) {
    const size_t N = d.idom.size();
    d.child_offs.assign(N + 1, DomChildSlot(0));
    d.children.clear();
    for (size_t b = 0; b < N; ++b)
        if (IrBlockId(b) != entry && d.idom[b] != ir::IR_NO_BLOCK)
            d.child_offs[d.idom[b] + 1] =
                DomChildSlot(d.child_offs[d.idom[b] + 1] + 1);
    for (size_t b = 0; b < N; ++b)
        d.child_offs[b + 1] = DomChildSlot(d.child_offs[b + 1] + d.child_offs[b]);
    d.children.resize(d.child_offs[N], IrBlockId(0));
    util::SmallVector<DomChildSlot, kDomInlineBlocks> next(N, DomChildSlot(0));
    for (size_t b = 0; b < N; ++b)
        next[b] = d.child_offs[b];
    for (size_t b = 0; b < N; ++b)
        if (IrBlockId(b) != entry && d.idom[b] != ir::IR_NO_BLOCK) {
            const IrBlockId p = d.idom[b];
            d.children[next[p]] = IrBlockId(b);
            next[p] = DomChildSlot(next[p] + 1);
        }
}

/**
 * @brief Numera el arbol de dominadores al entrar y al salir (iterativo).
 *
 * Es lo que deja contestar "domina" en tiempo CONSTANTE.  Subir por la cadena
 * de dominadores inmediatos era aristas por profundidad al buscar bucles: con
 * el codigo que deja el inliner el arbol es casi una cadena.
 */
void number_dom_tree(DomFacts &d, IrBlockId entry) {
    const size_t N = d.idom.size();
    d.pre.assign(N, NO_DOM_TICK);
    d.post.assign(N, NO_DOM_TICK);
    if (N == 0 || d.idom[entry] == ir::IR_NO_BLOCK) return;
    const auto &off = d.child_offs;
    const auto &kids = d.children;
    uint32_t clock = 0; // instantes repartidos
    util::SmallVector<DomWalkStep, kDomInlineBlocks> walk;
    d.pre[entry] = DomTick(clock++);
    walk.push_back({entry, off[entry]});
    while (!walk.empty()) {
        DomWalkStep &top = walk.back();
        if (top.next < off[top.block + 1]) {
            const IrBlockId c = kids[top.next];
            top.next = DomChildSlot(top.next + 1);
            d.pre[c] = DomTick(clock++);
            /* `top` ya no se usa: apilar puede mover la tabla. */
            walk.push_back({c, off[c]});
        } else {
            d.post[top.block] = DomTick(clock++);
            walk.pop_back();
        }
    }
}

} // namespace

DomFacts compute_dom_facts(const IrFunction &fn, ir::IrEdgeWant edges) {
    DomFacts d;
    d.edges = edges;
    const size_t N = fn.blocks.size();
    if (N == 0) return d;
    const IrBlockId entry = IrBlockId(0);
    d.succs = block_successors(fn, edges);
    d.preds = invert_block_graph(d.succs);
    compute_rpo(d.succs, entry, d.postorder, d.rpo);
    compute_idom(d.preds, d.postorder, d.rpo, entry, d.idom);
    build_dom_children(d, entry);
    number_dom_tree(d, entry);
    return d;
}

BlockGraph compute_dominance_frontier(const DomFacts &d) {
    const size_t N = d.block_count();
    /* Cytron: por cada bloque de UNION -- dos o mas predecesores --, se sube
     * desde cada predecesor por el arbol hasta el dominador inmediato de la
     * union, y cada bloque del camino la tiene en su frontera.  Se junta como
     * pares (bloque, frontera) y luego se reparte en plano. */
    struct DfPair {
        IrBlockId block;
        IrBlockId frontier;
    };
    util::SmallVector<DfPair, kDomInlineBlocks> pairs;
    /* La ultima union apuntada en la frontera de cada bloque: dos
     * predecesores cuyo camino pasa por el mismo bloque lo apuntarian dos
     * veces. */
    util::SmallVector<IrBlockId, kDomInlineBlocks> last(N, ir::IR_NO_BLOCK);
    const IrBlockId entry = IrBlockId(0);
    for (size_t bi = 0; bi < N; ++bi) {
        const IrBlockId b = IrBlockId(bi);
        if (!d.reachable(b)) continue;
        /* La ENTRADA tiene un predecesor mas que no se ve: el de fuera, por el
         * que se llega la primera vez (el "inicio" virtual de Cytron).  Asi que
         * es una union en cuanto algo salte a ella, y su dominador es ese
         * inicio: el recorrido la incluye.  Sin esto, algo que vuelva a la
         * entrada no tendria PHI alli y la segunda vuelta leeria el valor de
         * la primera. */
        const bool is_entry = b == entry;
        if (d.preds[b].size() < (is_entry ? 1u : 2u)) continue;
        for (IrBlockId p : d.preds[b]) {
            if (!d.reachable(p)) continue; // un camino que no existe
            IrBlockId runner = p;
            while (true) {
                if (!is_entry && runner == d.idom[b]) break;
                if (last[runner] != b) {
                    last[runner] = b;
                    pairs.push_back({runner, b});
                }
                if (runner == entry) break; // no se sube mas alla de la raiz
                runner = d.idom[runner];
            }
        }
    }
    BlockGraph df;
    df.offs.assign(N + 1, CfgEdgeSlot(0));
    for (const DfPair &pr : pairs)
        df.offs[pr.block + 1] = CfgEdgeSlot(df.offs[pr.block + 1] + 1);
    for (size_t b = 0; b < N; ++b)
        df.offs[b + 1] = CfgEdgeSlot(df.offs[b + 1] + df.offs[b]);
    df.edges.resize(df.offs[N], IrBlockId(0));
    util::SmallVector<CfgEdgeSlot, kDomInlineBlocks> next(N, CfgEdgeSlot(0));
    for (size_t b = 0; b < N; ++b)
        next[b] = df.offs[b];
    for (const DfPair &pr : pairs) {
        df.edges[next[pr.block]] = pr.frontier;
        next[pr.block] = CfgEdgeSlot(next[pr.block] + 1);
    }
    return df;
}

} // namespace analysis
