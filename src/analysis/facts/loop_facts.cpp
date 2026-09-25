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
 * Dominadores (de @c DomFacts, ya no calculados aqui) -> back-edges -> cuerpos
 * de bucle (BFS inverso) -> profundidad por bloque.
 */

#include "analysis/facts/loop_facts.h"

#include "util/alloc/small_vector.h" // las tablas de trabajo, sin reservar

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
 * Todas se dimensionan al numero de BLOQUES o de BUCLES, y una funcion normal
 * tiene pocos: viven en linea mientras quepan y solo despues tocan el monton.
 * Cuando esto reservaba en el monton era, el solo, el 5 % de todas las
 * reservas de compilar.
 * @{
 */
/// Lo mismo que usa @c DomFacts: la misma funcion normal cabe en las dos.
constexpr size_t kInlineBlocks = kDomInlineBlocks;
/// Una lista de bloques: cabeceras, latches, cuerpos, una pila de recorrido.
using BlockList = util::SmallVector<IrBlockId, kInlineBlocks>;
/// @}

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
    /* Sin base a la que preguntar: se calculan aqui.  Quien tenga la base
     * pide los dominadores por ella y llama a la otra forma, para que no se
     * calculen dos veces. */
    return compute_loop_facts(fn, compute_dom_facts(fn));
}

LoopFacts compute_loop_facts(const IrFunction &fn, const DomFacts &dom) {
    const size_t N = fn.blocks.size();
    LoopFacts f;
    f.loop_depth.assign(N, 0);
    f.is_loop_header.assign(N, 0);
    f.in_loop.assign(N, 0);
    f.loop_id.assign(N, LoopFacts::NO_LOOP);
    if (N == 0) return f;

    const BlockGraph &succs = dom.succs;
    const BlockGraph &preds = dom.preds;

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
