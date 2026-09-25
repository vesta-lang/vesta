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

/**
 * @brief El dominador comun mas cercano de @p a y @p b, subiendo por @p idom.
 *
 * Numeros de postorden mas ALTOS = mas cerca de la entrada en RPO; se sube por
 * el que este mas lejos hasta que se encuentran.
 */
IrBlockId idom_intersect(const BlockList &idom, const PostorderNums &po,
                         IrBlockId a, IrBlockId b) {
    while (a != b) {
        while (po[a] < po[b])
            a = idom[a];
        while (po[b] < po[a])
            b = idom[b];
    }
    return a;
}

/** @brief idom via CHK.  idom[b] = IR_NO_BLOCK si inalcanzable. */
BlockList compute_idom(const Graph &preds, const PostorderNums &po,
                       const BlockList &rpo, IrBlockId entry) {
    const size_t N = preds.size();
    BlockList idom(N, ir::IR_NO_BLOCK);
    if (rpo.empty()) return idom;
    idom[entry] = entry;

    bool changed = true;
    while (changed) {
        changed = false;
        for (IrBlockId b : rpo) {
            if (b == entry) continue;
            IrBlockId new_idom = ir::IR_NO_BLOCK;
            for (IrBlockId p : preds[b]) {
                if (idom[p] == ir::IR_NO_BLOCK)
                    continue; // pred aun sin procesar
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
    return idom;
}

/// Instante en que el recorrido del arbol de dominadores entra o sale de un
/// bloque.
enum DomTick : uint32_t {};
/// Un bloque inalcanzable: no esta en el arbol.
constexpr DomTick NO_DOM_TICK = DomTick(0xFFFFFFFFu);
/// Posicion en la lista aplanada de hijos del arbol.
enum DomChildSlot : uint32_t {};

/**
 * @brief El arbol de dominadores numerado: cuando se ENTRA y cuando se SALE de
 *        cada bloque en un recorrido en profundidad.
 *
 * Existe para contestar "domina" en tiempo CONSTANTE.  Antes se subia por la
 * cadena de dominadores inmediatos desde el bloque, y esa pregunta se hace por
 * cada arista al buscar bucles: con el codigo que deja el inliner el arbol es
 * casi una cadena, y eso era aristas por profundidad.
 */
struct DomNumbering {
    util::SmallVector<DomTick, kInlineBlocks> pre, post;

    /// @p a domina a @p b: @p b cae dentro del subarbol de @p a.
    bool dominates(IrBlockId a, IrBlockId b) const {
        if (pre[a] == NO_DOM_TICK || pre[b] == NO_DOM_TICK) return false;
        return pre[a] <= pre[b] && post[b] <= post[a];
    }
};

/// Un paso pendiente del recorrido que numera el arbol.
struct DomWalkStep {
    IrBlockId block;
    DomChildSlot next; ///< siguiente hijo por visitar.
};

/** @brief Numera el arbol de dominadores dado por @p idom (iterativo). */
DomNumbering number_dom_tree(const BlockList &idom, IrBlockId entry) {
    const size_t N = idom.size();
    DomNumbering d;
    d.pre.assign(N, NO_DOM_TICK);
    d.post.assign(N, NO_DOM_TICK);
    if (N == 0 || idom[entry] == ir::IR_NO_BLOCK) return d;
    /* Los hijos de cada bloque, contiguos. */
    util::SmallVector<DomChildSlot, kInlineBlocks + 1> off(N + 1,
                                                          DomChildSlot(0));
    for (size_t b = 0; b < N; ++b)
        if (IrBlockId(b) != entry && idom[b] != ir::IR_NO_BLOCK)
            off[idom[b] + 1] = DomChildSlot(off[idom[b] + 1] + 1);
    for (size_t b = 0; b < N; ++b)
        off[b + 1] = DomChildSlot(off[b + 1] + off[b]);
    BlockList kids;
    kids.resize(off[N], IrBlockId(0));
    {
        util::SmallVector<DomChildSlot, kInlineBlocks> next(N, DomChildSlot(0));
        for (size_t b = 0; b < N; ++b)
            next[b] = off[b];
        for (size_t b = 0; b < N; ++b)
            if (IrBlockId(b) != entry && idom[b] != ir::IR_NO_BLOCK) {
                const IrBlockId p = idom[b];
                kids[next[p]] = IrBlockId(b);
                next[p] = DomChildSlot(next[p] + 1);
            }
    }
    uint32_t clock = 0; // instantes repartidos
    util::SmallVector<DomWalkStep, kInlineBlocks> walk;
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
    return d;
}

/// Indice de un bucle mientras se construyen.
enum LoopIdx : uint32_t {};
/// Ningun bucle.
constexpr LoopIdx NO_LOOP_IDX = LoopIdx(0xFFFFFFFFu);
/// Posicion en las listas aplanadas por bucle (latches y cuerpos).
enum LoopSlot : uint32_t {};

/// Una arista de retroceso: del latch a la cabecera que lo domina.
struct BackEdge {
    IrBlockId latch;
    IrBlockId header;
};

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
    const DomNumbering dom = number_dom_tree(idom, entry);

    /* Aristas de retroceso (b -> h con h dominando b).  Cada cabecera es UN
     * bucle, numerado en el orden en que aparece su primera arista. */
    util::SmallVector<BackEdge, kInlineBlocks> back;
    util::SmallVector<LoopIdx, kInlineBlocks> loop_of_header(N, NO_LOOP_IDX);
    BlockList headers; // cabecera de cada bucle, por indice
    for (size_t b = 0; b < N; ++b)
        for (IrBlockId h : succs[b]) {
            if (!dom.dominates(h, IrBlockId(b))) continue;
            back.push_back({IrBlockId(b), h});
            if (loop_of_header[h] == NO_LOOP_IDX) {
                loop_of_header[h] = LoopIdx(headers.size());
                headers.push_back(h);
            }
        }
    const size_t n_loops = headers.size();

    /* Los latches de cada bucle, contiguos. */
    util::SmallVector<LoopSlot, kInlineBlocks + 1> latch_off(n_loops + 1,
                                                            LoopSlot(0));
    for (const BackEdge &e : back)
        latch_off[loop_of_header[e.header] + 1] =
            LoopSlot(latch_off[loop_of_header[e.header] + 1] + 1);
    for (size_t l = 0; l < n_loops; ++l)
        latch_off[l + 1] = LoopSlot(latch_off[l + 1] + latch_off[l]);
    BlockList latches;
    latches.resize(latch_off[n_loops], IrBlockId(0));
    {
        util::SmallVector<LoopSlot, kInlineBlocks> next(n_loops, LoopSlot(0));
        for (size_t l = 0; l < n_loops; ++l)
            next[l] = latch_off[l];
        for (const BackEdge &e : back) {
            const LoopIdx l = loop_of_header[e.header];
            latches[next[l]] = e.latch;
            next[l] = LoopSlot(next[l] + 1);
        }
    }

    /* El cuerpo de cada bucle como LISTA de sus bloques, no como un mapa del
     * tamano de la funcion.  Con un mapa por bucle, cada uno costaba lo que la
     * funcion entera -- al crearlo, al contarlo y al repartirlo --, y eran
     * bucles por bloques, en tiempo y en memoria.  La lista cuesta lo que el
     * cuerpo.  Desde todos los latches a la vez: BFS inverso por
     * predecesores, sin pasar de la cabecera.  La marca lleva el bucle en
     * curso, asi que no se limpia entre uno y el siguiente. */
    util::SmallVector<LoopIdx, kInlineBlocks> in_body(N, NO_LOOP_IDX);
    util::SmallVector<LoopSlot, kInlineBlocks + 1> body_off(n_loops + 1,
                                                           LoopSlot(0));
    BlockList body; // los cuerpos, uno tras otro
    BlockList stk;
    for (size_t l = 0; l < n_loops; ++l) {
        const LoopIdx li = LoopIdx(l);
        const IrBlockId h = headers[l];
        body_off[l] = LoopSlot(body.size());
        in_body[h] = li;
        body.push_back(h);
        stk.clear();
        for (LoopSlot s = latch_off[l]; s < latch_off[l + 1];
             s = LoopSlot(s + 1)) {
            const IrBlockId b = latches[s];
            if (in_body[b] == li) continue;
            in_body[b] = li;
            body.push_back(b);
            stk.push_back(b);
        }
        while (!stk.empty()) {
            const IrBlockId x = stk.back();
            stk.pop_back();
            for (IrBlockId p : preds[x])
                if (in_body[p] != li) {
                    in_body[p] = li;
                    body.push_back(p);
                    if (p != h) stk.push_back(p);
                }
        }
    }
    body_off[n_loops] = LoopSlot(body.size());

    /* Hechos por bloque, y el padre de cada bucle, recorriendo los cuerpos
     * una vez.  El bucle MAS INTERNO de un bloque es el de menor cuerpo que lo
     * contiene; el PADRE de un bucle, el de menor cuerpo que contiene
     * PROPIAMENTE a su cabecera.  Los empates se resuelven como siempre: gana
     * el de indice menor, que es el que se ve primero. */
    f.loop_count = static_cast<uint32_t>(n_loops);
    f.loop_header.resize(n_loops);
    f.parent_loop.assign(n_loops, LoopFacts::NO_LOOP);
    for (size_t l = 0; l < n_loops; ++l) {
        f.loop_header[l] = headers[l];
        f.is_loop_header[headers[l]] = 1;
    }
    for (size_t l = 0; l < n_loops; ++l) {
        const size_t size = body_off[l + 1] - body_off[l];
        for (LoopSlot s = body_off[l]; s < body_off[l + 1];
             s = LoopSlot(s + 1)) {
            const IrBlockId b = body[s];
            f.in_loop[b] = 1;
            f.loop_depth[b] += 1; // un bucle mas que contiene el bloque
            const uint32_t cur = f.loop_id[b];
            if (cur == LoopFacts::NO_LOOP ||
                size < size_t(body_off[cur + 1] - body_off[cur]))
                f.loop_id[b] = static_cast<uint32_t>(l);
            /* Si `b` es la cabecera de otro bucle, este lo contiene: es
             * candidato a padre si es PROPIAMENTE mayor. */
            const LoopIdx inner = loop_of_header[b];
            if (inner == NO_LOOP_IDX || size_t(inner) == l) continue;
            const size_t inner_size =
                body_off[inner + 1] - body_off[inner];
            if (size <= inner_size) continue;
            const uint32_t best = f.parent_loop[inner];
            if (best == LoopFacts::NO_LOOP ||
                size < size_t(body_off[best + 1] - body_off[best]))
                f.parent_loop[inner] = static_cast<uint32_t>(l);
        }
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
