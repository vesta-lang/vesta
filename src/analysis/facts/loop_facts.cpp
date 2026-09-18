/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/facts/loop_facts.cpp
 * @brief Implementacion de @c compute_loop_facts (ver loop_facts.h).
 *
 * CFG desde terminadores -> dominadores (Cooper-Harvey-Kennedy) -> back-edges
 * -> cuerpos de bucle (BFS inverso) -> profundidad por bloque.  Mismo algoritmo
 * canonico que SROA/LICM usaban por separado, ahora unificado.
 */

#include "analysis/facts/loop_facts.h"

#include "util/alloc/small_vector.h" // los vecinos de un bloque, sin reservar
#include "util/named_alloc.h"        // que el perfil diga QUE es cada tabla

#include <cstdint>
#include <vector>

namespace analysis {

/// La identidad del analisis para el gestor.  Aqui, con su dominio, como los
/// demas; estaba escondida en `fact_base.cpp`.
char LoopsAnalysis::ID = 0;

using ir::IrBlockId;
using ir::IrFunction;

namespace {

/**
 * @name Las tablas de este analisis, EN LA PILA
 *
 * Este fichero era, el solo, OCHO sitios de 168.007 reservas cada uno -- una
 * por tabla y siete visitas por funcion --: 1,34 millones, el 5% de todo lo
 * que reserva compilar.  Y ninguna era grande: casi todas de uno a veinticuatro
 * bytes, porque todas se dimensionan al numero de BLOQUES y una funcion normal
 * tiene unos pocos.
 *
 * Son estructuras de trabajo que nacen y mueren dentro de una llamada, asi que
 * ahora viven en la PILA mientras quepan.  @c kInlineBlocks es cuantos bloques
 * caben antes de tocar el monton; pasado eso crecen como cualquier vector y
 * todo sigue igual.
 *
 * LO QUE SE PIERDE, y se dice: tres de ellas llevaban etiqueta
 * (@c util::NamedVector) para que el perfil supiera cual era cual, porque
 * byte a byte son identicas a las otras ciento sesenta tablas de cuatro bytes
 * del arbol.  La etiqueta estaba para ENCONTRARLAS; encontradas y quitadas, lo
 * que queda es el resto de las funciones grandes.  Si ese resto llega a pesar,
 * la etiqueta vuelve.
 * @{
 */

/// Bloques que caben en la pila antes de que una tabla toque el monton.
/// Ocho cubre la funcion normal; el `main` de un programa de verdad no, y por
/// eso las tablas siguen sabiendo crecer.
constexpr size_t kInlineBlocks = 8;

/**
 * @brief Los vecinos de UN bloque, con los dos primeros dentro de la tabla.
 *
 * Dos, porque eso es lo que tiene un bloque: el que sigue y el del salto.
 */
using Neighbors = util::SmallVector<IrBlockId, 2>;
/// Bloque -> su numero de postorden.
using PostorderNums = util::SmallVector<uint32_t, kInlineBlocks>;
/// Cabecera -> indice de su bucle.
using LoopOfHeader = util::SmallVector<int32_t, kInlineBlocks>;
/// Que bloques forman el cuerpo de un bucle.
using LoopBody = util::SmallVector<uint8_t, kInlineBlocks>;
/// Una lista de bloques: el postorden, su inverso, una pila de recorrido.
using BlockList = util::SmallVector<IrBlockId, kInlineBlocks>;
/// @}

/**
 * @brief El grafo de bloques, en DOS tablas contiguas en vez de una por
 *        bloque.
 *
 * Los vecinos del bloque `b` son `edges[offs[b] .. offs[b+1])`.  Antes esto
 * era un vector de vectores, y ahi cada bloque pagaba SU reserva la primera
 * vez que se le anadia un vecino: el grafo de una funcion costaba tantas
 * reservas como bloques, dos veces -- sucesores y predecesores --.
 *
 * Ademas de no reservar, se recorre en orden: los vecinos de todos los bloques
 * estan seguidos en memoria, que es lo que pide la regla de estructuras del
 * proyecto para un camino que se pasa la vida mirando tablas.
 */
struct Graph {
    /// Donde empieza cada bloque dentro de @c edges.  Tiene N+1 entradas.
    util::SmallVector<uint32_t, kInlineBlocks + 1> offs;
    /// Los vecinos de todos los bloques, seguidos.
    util::SmallVector<IrBlockId, kInlineBlocks * 2> edges;

    /// Cuantos bloques hay.
    size_t size() const noexcept { return offs.empty() ? 0 : offs.size() - 1; }

    /// Los vecinos de UN bloque, como algo que se puede recorrer e indexar.
    struct Row {
        const IrBlockId *first;
        const IrBlockId *last;
        const IrBlockId *begin() const noexcept { return first; }
        const IrBlockId *end() const noexcept { return last; }
        size_t size() const noexcept {
            return static_cast<size_t>(last - first);
        }
        IrBlockId operator[](size_t i) const noexcept { return first[i]; }
    };

    Row operator[](size_t b) const noexcept {
        const IrBlockId *base = edges.begin();
        return Row{base + offs[b], base + offs[b + 1]};
    }
};

/** @brief Anade @p t a @p v si es un bloque valido y no estaba ya. */
void add_neighbor(Neighbors &v, IrBlockId t, size_t N) {
    if (t == ir::IR_NO_BLOCK || static_cast<size_t>(t) >= N) return;
    for (IrBlockId x : v)
        if (x == t) return; // dedup
    v.push_back(t);
}

/** @brief Sucesores de cada bloque, tomados de los terminadores. */
Graph build_succs(const IrFunction &fn) {
    const size_t N = fn.blocks.size();
    Graph succs;
    succs.offs.assign(N + 1, 0);
    // Los vecinos del bloque en curso se juntan aqui -- hace falta para
    // deduplicar -- y esta fila se REUSA entre bloques, asi que el
    // almacenamiento en linea se paga una vez y no una por bloque.
    Neighbors row;
    for (size_t b = 0; b < N; ++b) {
        row.clear();
        for (const ir::IrInstr &ins : fn.blocks[b].instrs) {
            add_neighbor(row, ins.target_block, N);
            add_neighbor(row, ins.false_block, N);
            for (uint32_t jt : ins.jump_targets)
                add_neighbor(row, static_cast<IrBlockId>(jt), N);
        }
        for (IrBlockId s : row)
            succs.edges.push_back(s);
        succs.offs[b + 1] = static_cast<uint32_t>(succs.edges.size());
    }
    return succs;
}

/** @brief Predecesores = inversa de los sucesores. */
Graph build_preds(const Graph &succs) {
    const size_t N = succs.size();
    Graph preds;
    // Contar cuantos predecesores tiene cada bloque, en offs[b+1]...
    preds.offs.assign(N + 1, 0);
    for (size_t b = 0; b < N; ++b)
        for (IrBlockId s : succs[b])
            preds.offs[static_cast<size_t>(s) + 1]++;
    // ...y convertir las cuentas en donde empieza cada uno.
    for (size_t b = 0; b < N; ++b)
        preds.offs[b + 1] += preds.offs[b];
    preds.edges.resize(preds.offs[N], IrBlockId(0));
    // Cursor por bloque: cuantos lleva colocados ya.
    util::SmallVector<uint32_t, kInlineBlocks> placed(N, 0);
    for (size_t b = 0; b < N; ++b)
        for (IrBlockId s : succs[b]) {
            const size_t si = static_cast<size_t>(s);
            preds.edges[preds.offs[si] + placed[si]++] =
                static_cast<IrBlockId>(b);
        }
    return preds;
}

/**
 * @brief Numeracion postorden por DFS desde @p entry (iterativo).
 * @param po      salida: po[b] = numero postorden, o UINT32_MAX si
 * inalcanzable.
 * @param rpo     salida: bloques en reverse-postorden (solo alcanzables).
 */
void compute_rpo(const Graph &succs, IrBlockId entry, PostorderNums &po,
                 BlockList &rpo) {
    const size_t N = succs.size();
    po.assign(N, UINT32_MAX);
    // Bloques ya vistos por el recorrido en profundidad.
    util::SmallVector<uint8_t, kInlineBlocks> visited(N, 0);
    BlockList order; // postorden
    // DFS iterativo con pila de (nodo, indice de sucesor).  Un struct propio y
    // no un `std::pair`, que no es trivialmente copiable y esta tabla se mueve
    // con `memcpy`.
    struct Visit {
        IrBlockId block; ///< donde esta el recorrido.
        uint32_t next;   ///< por que sucesor suyo va.
    };
    util::SmallVector<Visit, kInlineBlocks> stk;
    if (static_cast<size_t>(entry) >= N) return;
    visited[entry] = 1;
    stk.push_back({entry, 0});
    while (!stk.empty()) {
        Visit &top = stk.back();
        if (top.next < succs[top.block].size()) {
            IrBlockId s = succs[top.block][top.next++];
            if (!visited[s]) {
                visited[s] = 1;
                stk.push_back({s, 0});
            }
        } else {
            order.push_back(top.block);
            stk.pop_back();
        }
    }
    uint32_t n = 0;
    for (IrBlockId b : order)
        po[b] = n++;
    // RPO = orden inverso del postorden.  A mano y no con iteradores inversos,
    // que esta tabla no tiene: copiar del final al principio es lo mismo.
    rpo.clear();
    rpo.reserve(order.size());
    for (size_t i = order.size(); i-- > 0;)
        rpo.push_back(order[i]);
}

/** @brief idom via CHK.  idom[b] = IR_NO_BLOCK si inalcanzable. */
BlockList compute_idom(const Graph &preds, const PostorderNums &po,
                       const BlockList &rpo, IrBlockId entry) {
    const size_t N = preds.size();
    BlockList idom(N, ir::IR_NO_BLOCK);
    if (rpo.empty()) return idom;
    idom[entry] = entry;

    auto intersect = [&](IrBlockId a, IrBlockId b) -> IrBlockId {
        while (a != b) {
            // Numeros postorden mas ALTOS = mas cerca de la entrada en RPO.
            while (po[a] < po[b])
                a = idom[a];
            while (po[b] < po[a])
                b = idom[b];
        }
        return a;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (IrBlockId b : rpo) {
            if (b == entry) continue;
            IrBlockId new_idom = ir::IR_NO_BLOCK;
            for (IrBlockId p : preds[b]) {
                if (idom[p] == ir::IR_NO_BLOCK)
                    continue; // pred aun sin procesar
                new_idom =
                    (new_idom == ir::IR_NO_BLOCK) ? p : intersect(p, new_idom);
            }
            if (new_idom != ir::IR_NO_BLOCK && idom[b] != new_idom) {
                idom[b] = new_idom;
                changed = true;
            }
        }
    }
    return idom;
}

/** @brief True si @p a domina a @p b (recorre la cadena idom de @p b). */
bool dominates(const BlockList &idom, IrBlockId a, IrBlockId b) {
    if (idom[b] == ir::IR_NO_BLOCK) return false; // b inalcanzable
    IrBlockId cur = b;
    while (true) {
        if (cur == a) return true;
        if (idom[cur] == cur)
            return false; // llego a la entrada sin encontrar a
        cur = idom[cur];
    }
}

} // namespace

LoopFacts compute_loop_facts(const IrFunction &fn) {
    const size_t N = fn.blocks.size();
    LoopFacts f;
    f.loop_depth.assign(N, 0);
    f.is_loop_header.assign(N, 0);
    f.in_loop.assign(N, 0);
    f.loop_id.assign(N, LoopFacts::NO_LOOP);
    if (N == 0) return f;

    const IrBlockId entry = IrBlockId(0);
    auto succs = build_succs(fn);
    auto preds = build_preds(succs);
    PostorderNums po;
    BlockList rpo;
    compute_rpo(succs, entry, po, rpo);
    auto idom = compute_idom(preds, po, rpo, entry);

    // Back-edges (b -> h con h dominando b), agrupados por cabecera = 1 bucle.
    struct Loop {
        IrBlockId header;
        LoopBody body;
        size_t size = 0;
    };
    std::vector<Loop> loops;
    LoopOfHeader loop_of_header(N, -1); // header -> indice en loops

    for (size_t b = 0; b < N; ++b) {
        for (IrBlockId h : succs[b]) {
            if (!dominates(idom, h, static_cast<IrBlockId>(b))) continue;
            // Back-edge b->h.  Obtener/crear el bucle de cabecera h.
            int32_t li = loop_of_header[h];
            if (li < 0) {
                loops.push_back(Loop{h, LoopBody(N, 0), 0});
                li = static_cast<int32_t>(loops.size()) - 1;
                loop_of_header[h] = li;
                loops[li].body[h] = 1;
            }
            Loop &lp = loops[li];
            // Cuerpo: BFS inverso desde b por preds, sin pasar de h.
            BlockList stk;
            if (!lp.body[b]) {
                lp.body[b] = 1;
                stk.push_back(static_cast<IrBlockId>(b));
            }
            while (!stk.empty()) {
                IrBlockId x = stk.back();
                stk.pop_back();
                if (x == h) continue;
                for (IrBlockId p : preds[x])
                    if (!lp.body[p]) {
                        lp.body[p] = 1;
                        if (p != h) stk.push_back(p);
                    }
            }
        }
    }

    // Tamanos + hechos por bloque.
    for (Loop &lp : loops) {
        lp.size = 0;
        for (size_t b = 0; b < N; ++b)
            lp.size += lp.body[b];
    }
    f.loop_count = static_cast<uint32_t>(loops.size());
    for (size_t li = 0; li < loops.size(); ++li) {
        const Loop &lp = loops[li];
        f.is_loop_header[lp.header] = 1;
        for (size_t b = 0; b < N; ++b) {
            if (!lp.body[b]) continue;
            f.in_loop[b] = 1;
            f.loop_depth[b] += 1; // un bucle mas que contiene el bloque
            // loop_id = bucle MAS INTERNO (menor cuerpo) que contiene el
            // bloque.
            uint32_t cur = f.loop_id[b];
            if (cur == LoopFacts::NO_LOOP || lp.size < loops[cur].size)
                f.loop_id[b] = static_cast<uint32_t>(li);
        }
    }

    // Hechos POR BUCLE: cabecera + bucle padre (el bucle mas pequeno que
    // CONTIENE PROPIAMENTE a este = su cabecera cae en el cuerpo de otro
    // mayor).
    f.loop_header.resize(loops.size());
    f.parent_loop.assign(loops.size(), LoopFacts::NO_LOOP);
    for (size_t li = 0; li < loops.size(); ++li) {
        f.loop_header[li] = loops[li].header;
        const IrBlockId h = loops[li].header;
        uint32_t best = LoopFacts::NO_LOOP;
        size_t best_size = SIZE_MAX;
        for (size_t lj = 0; lj < loops.size(); ++lj) {
            if (lj == li) continue;
            if (!loops[lj].body[h]) continue; // lj contiene la cabecera de li
            if (loops[lj].size <= loops[li].size)
                continue; // contencion PROPIA (mayor)
            if (loops[lj].size < best_size) {
                best_size = loops[lj].size;
                best = static_cast<uint32_t>(lj);
            }
        }
        f.parent_loop[li] = best;
    }
    return f;
}

std::vector<FactIssue> validate(const LoopFacts &f) {
    std::vector<FactIssue> issues;
    const size_t N = f.loop_depth.size();
    for (size_t b = 0; b < N; ++b) {
        const bool in = f.in_loop[b] != 0;
        const bool depth_pos = f.loop_depth[b] > 0;
        // depth>0 <=> in_loop.
        if (in != depth_pos)
            issues.push_back({FactCheck::LOOP_DEPTH_INLOOP_MISMATCH, b, 0});
        // Un header debe estar in_loop.
        if (f.is_loop_header[b] && !in)
            issues.push_back({FactCheck::LOOP_HEADER_NOT_IN_LOOP, b, 0});
        // loop_id en rango.
        if (f.loop_id[b] != LoopFacts::NO_LOOP && f.loop_id[b] >= f.loop_count)
            issues.push_back(
                {FactCheck::LOOP_ID_OUT_OF_RANGE, b, f.loop_id[b]});
    }
    for (uint32_t L = 0; L < f.loop_count; ++L) {
        const uint32_t p = f.parent_of(L);
        if (p != LoopFacts::NO_LOOP && p >= f.loop_count)
            issues.push_back({FactCheck::LOOP_PARENT_OUT_OF_RANGE, L, p});
        if (p == L) issues.push_back({FactCheck::LOOP_PARENT_SELF, L, 0});
        if (L < f.loop_header.size() && f.loop_header[L] >= N)
            issues.push_back(
                {FactCheck::LOOP_HEADER_BLOCK_OOR, L, f.loop_header[L]});
    }
    return issues;
}

} // namespace analysis
