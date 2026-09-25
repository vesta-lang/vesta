/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/analysis/test_dom_facts.cpp
 * @brief @c DomFacts y la regla unica de aristas, contra oraculos ingenuos.
 *
 * Tres cosas, cada una contra una version escrita a lo bruto y sin compartir
 * codigo con el productor:
 *
 *  1. @c IrFunction::edges_of y @c recompute_succs_of: que las aristas
 *     NORMALES salgan exactamente como antes -- en el orden del terminador y
 *     sin quitar repetidos, porque de eso cuelgan los argumentos de las PHI
 *     -- y que las EXCEPCIONALES sean el manejador de cada TRYENTER.
 *  2. El grafo de @c DomFacts: las dos clases, sin repetidos, y los
 *     predecesores como su inversa exacta.
 *  3. Los dominadores: el oraculo es el punto fijo de libro sobre CONJUNTOS
 *     (Dom(n) = {n} + interseccion de Dom(p)), que no se parece en nada a
 *     Cooper-Harvey-Kennedy.  Se compara `dominates` para TODOS los pares, el
 *     dominador inmediato, el alcance y el orden RPO.
 *
 * Sobre miles de funciones AL AZAR: autolazos, bloques vacios, inalcanzables,
 * destinos fuera de la funcion, aristas repetidas, varios TRYENTER por bloque
 * y grafos IRREDUCIBLES -- que es donde un algoritmo de dominadores rapido se
 * equivoca si se equivoca --.
 */
#include "analysis/facts/dom_facts.h"
#include "analysis/facts/loop_facts.h"
#include "ir/ssa_ir.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

using ir::IrBlockId;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fail;                                                          \
            std::printf("FALLO [%s:%d]: %s\n", __FILE__, __LINE__, msg);       \
        }                                                                      \
    } while (0)

// --------------------------------------------------------------------------
//  Generador determinista (sin <random>: el mismo resultado en todo sitio).
// --------------------------------------------------------------------------
struct Rng {
    uint64_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<uint32_t>(s);
    }
    uint32_t below(uint32_t n) { return n == 0 ? 0 : next() % n; }
};

/// Un destino al azar: casi siempre un bloque; a veces fuera de la funcion o
/// ninguno, que no deben producir arista.
static IrBlockId random_target(Rng &r, uint32_t nb) {
    const uint32_t k = r.below(20);
    if (k == 0) return ir::IR_NO_BLOCK;
    if (k == 1) return IrBlockId(nb + r.below(3)); // fuera de la funcion
    return IrBlockId(r.below(nb));
}

static ir::IrInstr make_op(ir::IrOp op) {
    ir::IrInstr in{};
    in.op = op;
    in.type = ir::IrType::VOID;
    in.dst = ir::IR_NO_VALUE;
    return in;
}

static ir::IrFunction random_function(Rng &r) {
    ir::IrFunction fn;
    fn.name = "rnd";
    const uint32_t nb = 1 + r.below(24);
    for (uint32_t b = 0; b < nb; ++b)
        fn.new_block();
    for (uint32_t b = 0; b < nb; ++b) {
        const IrBlockId bid = IrBlockId(b);
        if (r.below(12) == 0) continue; // bloque vacio: sin aristas
        /* Algun TRYENTER, en cualquier sitio del bloque. */
        const uint32_t n_try = r.below(4) == 0 ? 1 + r.below(2) : 0;
        for (uint32_t t = 0; t < n_try; ++t) {
            ir::IrInstr te = make_op(ir::IrOp::TRYENTER);
            te.target_block = random_target(r, nb);
            fn.append(bid, te);
            if (r.below(2)) fn.append(bid, make_op(ir::IrOp::NOP));
        }
        /* A veces un salto EN MEDIO del bloque, como el que deja elevar un
         * `asm` o la marca `SWITCH_DENSE` delante de su cadena. */
        if (r.below(6) == 0) {
            ir::IrInstr mid = make_op(r.below(2) ? ir::IrOp::BR
                                                 : ir::IrOp::SWITCH_DENSE);
            mid.target_block = random_target(r, nb);
            if (mid.op == ir::IrOp::SWITCH_DENSE)
                for (uint32_t i = 0, n = r.below(4); i < n; ++i)
                    mid.jump_targets.push_back(random_target(r, nb));
            fn.append(bid, mid);
            fn.append(bid, make_op(ir::IrOp::NOP));
        }
        switch (r.below(5)) {
        case 0: {
            ir::IrInstr t = make_op(ir::IrOp::BR);
            t.target_block = random_target(r, nb);
            fn.append(bid, t);
            break;
        }
        case 1:
        case 2: {
            ir::IrInstr t = make_op(ir::IrOp::BR_COND);
            t.target_block = random_target(r, nb);
            /* A veces las dos ramas al mismo sitio: arista repetida. */
            t.false_block = r.below(4) == 0 ? t.target_block
                                            : random_target(r, nb);
            fn.append(bid, t);
            break;
        }
        case 3: {
            ir::IrInstr t = make_op(ir::IrOp::SWITCH_DENSE);
            t.target_block = random_target(r, nb);
            const uint32_t n = r.below(5);
            for (uint32_t i = 0; i < n; ++i)
                t.jump_targets.push_back(random_target(r, nb));
            fn.append(bid, t);
            break;
        }
        default: fn.append(bid, make_op(ir::IrOp::RET)); break;
        }
    }
    return fn;
}

// --------------------------------------------------------------------------
//  Oraculos
// --------------------------------------------------------------------------

/// Las aristas NORMALES como las hacia `recompute_succs_of` antes del cambio:
/// solo el ultimo, en su orden, con repetidos, sin destinos fuera.
static std::vector<IrBlockId> oracle_normal(const ir::IrFunction &fn,
                                            IrBlockId b) {
    std::vector<IrBlockId> out;
    const ir::IrBlock &blk = fn.blocks[b];
    if (blk.instrs.empty()) return out;
    const size_t N = fn.blocks.size();
    const ir::IrInstr &t = blk.instrs.back();
    std::vector<IrBlockId> raw;
    if (t.op == ir::IrOp::BR) {
        raw.push_back(t.target_block);
    } else if (t.op == ir::IrOp::BR_COND) {
        raw.push_back(t.target_block);
        raw.push_back(t.false_block);
    } else if (t.op == ir::IrOp::SWITCH_DENSE) {
        raw.push_back(t.target_block);
        for (IrBlockId s : t.jump_targets)
            raw.push_back(s);
    }
    for (IrBlockId s : raw)
        if (s != ir::IR_NO_BLOCK && size_t(s) < N) out.push_back(s);
    return out;
}

/// Marca en @p row el destino @p s si es un bloque de la funcion.
static void mark_target(std::vector<bool> &row, IrBlockId s) {
    if (s != ir::IR_NO_BLOCK && size_t(s) < row.size()) row[s] = true;
}

/// Los sucesores del grafo de ANALISIS, SIN repetidos: todo lo que salta en
/// cualquier sitio del bloque -- terminador, saltos en medio, manejadores de
/// `TRYENTER` --.
static std::vector<std::vector<bool>> oracle_graph(const ir::IrFunction &fn) {
    const size_t N = fn.blocks.size();
    std::vector<std::vector<bool>> g(N, std::vector<bool>(N, false));
    for (size_t b = 0; b < N; ++b)
        for (const ir::IrInstr &in : fn.blocks[b].instrs) {
            if (in.op == ir::IrOp::TRYENTER || in.op == ir::IrOp::BR) {
                mark_target(g[b], in.target_block);
            } else if (in.op == ir::IrOp::BR_COND) {
                mark_target(g[b], in.target_block);
                mark_target(g[b], in.false_block);
            } else if (in.op == ir::IrOp::SWITCH_DENSE) {
                mark_target(g[b], in.target_block);
                for (IrBlockId s : in.jump_targets)
                    mark_target(g[b], s);
            }
        }
    return g;
}

/// Dom(n) de libro: punto fijo sobre conjuntos, solo en lo alcanzable.
static std::vector<std::vector<bool>>
oracle_dom(const std::vector<std::vector<bool>> &g,
           std::vector<bool> &reach) {
    const size_t N = g.size();
    reach.assign(N, false);
    if (N == 0) return {};
    std::vector<size_t> work{0};
    reach[0] = true;
    while (!work.empty()) {
        const size_t x = work.back();
        work.pop_back();
        for (size_t s = 0; s < N; ++s)
            if (g[x][s] && !reach[s]) {
                reach[s] = true;
                work.push_back(s);
            }
    }
    std::vector<std::vector<bool>> dom(N, std::vector<bool>(N, false));
    for (size_t n = 0; n < N; ++n)
        if (reach[n])
            for (size_t m = 0; m < N; ++m)
                dom[n][m] = (n == 0) ? (m == 0) : reach[m];
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t n = 1; n < N; ++n) {
            if (!reach[n]) continue;
            std::vector<bool> nd(N, true);
            for (size_t p = 0; p < N; ++p)
                if (reach[p] && g[p][n])
                    for (size_t m = 0; m < N; ++m)
                        nd[m] = nd[m] && dom[p][m];
            nd[n] = true;
            for (size_t m = 0; m < N; ++m)
                if (!reach[m]) nd[m] = false;
            if (nd != dom[n]) {
                dom[n] = nd;
                changed = true;
            }
        }
    }
    return dom;
}

// --------------------------------------------------------------------------
//  Comprobaciones sobre UNA funcion
// --------------------------------------------------------------------------
static void check_function(const ir::IrFunction &fn_in) {
    ir::IrFunction fn = fn_in;
    const size_t N = fn.blocks.size();

    /* 1. Las aristas normales del IR: exactamente las de antes. */
    fn.recompute_edges();
    for (size_t b = 0; b < N; ++b) {
        const std::vector<IrBlockId> want = oracle_normal(fn, IrBlockId(b));
        CHECK(fn.blocks[b].succs == want,
              "recompute_succs_of cambio las aristas normales");
    }

    /* 2. El grafo de analisis: las dos clases, sin repetidos; y la inversa. */
    const std::vector<std::vector<bool>> g = oracle_graph(fn);
    const analysis::DomFacts d = analysis::compute_dom_facts(fn);
    CHECK(d.succs.size() == N && d.preds.size() == N,
          "el grafo no tiene un bloque por bloque");
    for (size_t b = 0; b < N; ++b) {
        std::vector<bool> seen(N, false);
        bool dup = false;
        for (IrBlockId s : d.succs[b]) {
            if (seen[s]) dup = true;
            seen[s] = true;
        }
        CHECK(!dup, "sucesor repetido en el grafo de analisis");
        CHECK(seen == g[b], "sucesores distintos del oraculo");
        for (IrBlockId p : d.preds[b])
            CHECK(g[p][b], "predecesor que no tiene arista");
        size_t n_preds = 0;
        for (size_t p = 0; p < N; ++p)
            n_preds += g[p][b] ? 1 : 0;
        CHECK(d.preds[b].size() == n_preds, "faltan o sobran predecesores");
    }

    /* 3. Alcance, RPO y dominadores. */
    std::vector<bool> reach;
    const std::vector<std::vector<bool>> dom = oracle_dom(g, reach);
    size_t n_reach = 0;
    for (size_t b = 0; b < N; ++b) {
        CHECK(d.reachable(IrBlockId(b)) == bool(reach[b]),
              "alcance distinto del oraculo");
        n_reach += reach[b] ? 1 : 0;
    }
    CHECK(d.rpo.size() == n_reach, "el RPO no tiene todos los alcanzables");
    if (!d.rpo.empty()) CHECK(d.rpo[0] == IrBlockId(0), "el RPO no empieza en la entrada");
    std::vector<size_t> pos(N, SIZE_MAX);
    for (size_t i = 0; i < d.rpo.size(); ++i)
        pos[d.rpo[i]] = i;
    for (size_t b = 1; b < N; ++b) {
        /* Solo si los dos lo dan por alcanzable: si discrepan ya se dijo
         * arriba, y aqui `idom` no seria un indice valido. */
        if (!reach[b] || !d.reachable(IrBlockId(b))) continue;
        const size_t i = d.idom[b];
        CHECK(i < N && pos[i] < pos[b],
              "un dominador inmediato va despues en el RPO");
    }
    for (size_t a = 0; a < N; ++a)
        for (size_t b = 0; b < N; ++b) {
            const bool want = reach[a] && reach[b] && dom[b][a];
            CHECK(d.dominates(IrBlockId(a), IrBlockId(b)) == want,
                  "dominates distinto del oraculo");
        }
    /* El inmediato: domina estrictamente a b y lo dominan todos los demas
     * dominadores estrictos de b. */
    for (size_t b = 1; b < N; ++b) {
        if (!reach[b] || !d.reachable(IrBlockId(b))) continue;
        const size_t i = d.idom[b];
        bool ok = i < N && i != b && dom[b][i];
        for (size_t m = 0; ok && m < N; ++m)
            if (m != b && dom[b][m] && !dom[i][m]) ok = false;
        CHECK(ok, "dominador inmediato equivocado");
    }

    /* 4. Los hijos del arbol: los de `b` son los que tienen a `b` de
     * dominador inmediato, en orden de bloque. */
    for (size_t b = 0; b < N; ++b) {
        std::vector<IrBlockId> want;
        for (size_t c = 1; c < N; ++c)
            if (d.reachable(IrBlockId(c)) && d.idom[c] == IrBlockId(b))
                want.push_back(IrBlockId(c));
        const analysis::BlockGraph::Row got = d.children_of(IrBlockId(b));
        CHECK(std::vector<IrBlockId>(got.begin(), got.end()) == want,
              "hijos del arbol distintos de los que marca idom");
    }

    /* 5. La frontera de dominancia contra su DEFINICION: y esta en DF(x) si x
     * domina a un predecesor de y y no domina ESTRICTAMENTE a y. */
    const analysis::BlockGraph df = analysis::compute_dominance_frontier(d);
    for (size_t x = 0; x < N; ++x) {
        std::vector<bool> want(N, false);
        if (reach[x])
            for (size_t y = 0; y < N; ++y) {
                if (!reach[y]) continue;
                bool dom_pred = false;
                for (size_t p = 0; p < N; ++p)
                    if (reach[p] && g[p][y] && dom[p][x]) dom_pred = true;
                const bool strict = dom[y][x] && x != y;
                want[y] = dom_pred && !strict;
            }
        std::vector<bool> got(N, false);
        bool dup = false;
        for (IrBlockId y : df[x]) {
            if (got[y]) dup = true;
            got[y] = true;
        }
        CHECK(!dup, "frontera de dominancia con repetidos");
        CHECK(got == want, "frontera de dominancia distinta de la definicion");
        /* El primer desacuerdo, entero: sin el, un fallo aqui dice que algo
         * esta mal pero no QUE. */
        static bool shown = false;
        if (got != want && !shown) {
            shown = true;
            std::printf("  DF(%zu): N=%zu\n", x, N);
            for (size_t y = 0; y < N; ++y)
                if (got[y] != want[y])
                    std::printf("    y=%zu got=%d want=%d preds=%zu idom=%u\n",
                                y, int(got[y]), int(want[y]),
                                d.preds[y].size(), unsigned(d.idom[y]));
            for (size_t b = 0; b < N; ++b) {
                std::printf("    b%zu idom=%u ->", b, unsigned(d.idom[b]));
                for (IrBlockId s : d.succs[b])
                    std::printf(" %u", unsigned(s));
                std::printf("\n");
            }
        }
    }

    /* 6. El grafo de solo terminadores: las aristas de las PHI, sin repetidos.
     */
    const analysis::DomFacts dt =
        analysis::compute_dom_facts(fn, ir::IrEdgeWant::TerminatorOnly);
    for (size_t b = 0; b < N; ++b) {
        std::vector<bool> want(N, false), got(N, false);
        for (IrBlockId s : oracle_normal(fn, IrBlockId(b)))
            want[s] = true;
        for (IrBlockId s : dt.succs[b])
            got[s] = true;
        CHECK(got == want, "grafo de terminadores distinto de las aristas PHI");
    }

    /* 7. Los bucles con dominadores prestados = los de siempre. */
    const analysis::LoopFacts a = analysis::compute_loop_facts(fn);
    const analysis::LoopFacts b = analysis::compute_loop_facts(fn, d);
    CHECK(a.loop_depth == b.loop_depth && a.loop_id == b.loop_id &&
              a.loop_header == b.loop_header &&
              a.parent_loop == b.parent_loop,
          "los bucles cambian segun de donde salgan los dominadores");
}

/// El caso por el que existe la arista excepcional: un `catch` solo se alcanza
/// por ella.
static void check_catch_is_reachable() {
    ir::IrFunction fn;
    fn.name = "try_catch";
    const IrBlockId entry = fn.new_block("entry");
    const IrBlockId body = fn.new_block("body");
    const IrBlockId handler = fn.new_block("catch");
    ir::IrInstr te = make_op(ir::IrOp::TRYENTER);
    te.target_block = handler;
    fn.append(entry, te);
    ir::IrInstr br = make_op(ir::IrOp::BR);
    br.target_block = body;
    fn.append(entry, br);
    fn.append(body, make_op(ir::IrOp::RET));
    fn.append(handler, make_op(ir::IrOp::RET));
    fn.recompute_edges();

    CHECK(fn.blocks[entry].succs.size() == 1 &&
              fn.blocks[entry].succs[0] == body,
          "la arista del catch no debe entrar en las aristas de las PHI");
    const analysis::DomFacts d = analysis::compute_dom_facts(fn);
    CHECK(d.reachable(handler), "el catch tiene que ser alcanzable");
    CHECK(d.dominates(entry, handler), "la entrada domina al catch");
    CHECK(!d.dominates(body, handler), "el cuerpo NO domina al catch");
    CHECK(d.reachable(handler) && d.idom[handler] == entry,
          "el dominador inmediato del catch");
}

/// El verificador acusa un `tryenter` cuyo bloque y cuya direccion con nombre
/// no hablan del mismo manejador, y calla cuando si.  Es lo que permite que
/// la regla de aristas mire solo el bloque.
static void check_verify_tryenter_label() {
    for (int mismatch = 0; mismatch < 2; ++mismatch) {
        ir::IrModule mod;
        ir::IrFunction fn;
        fn.name = "f";
        const IrBlockId entry = fn.new_block("entry");
        const IrBlockId body = fn.new_block("body");
        const IrBlockId handler = fn.new_block("catch");
        const ir::IrValueId pc = fn.new_value(ir::IrType::PTR);
        const ir::IrValueId ty = fn.new_value(ir::IrType::I64);
        ir::IrInstr la = make_op(ir::IrOp::LABEL_ADDR);
        la.type = ir::IrType::PTR;
        la.dst = pc;
        // new_block anade un sufijo al nombre: la etiqueta sale del nombre real.
        la.func_name = fn.name + "_" + fn.blocks[mismatch ? body : handler].name;
        fn.append(entry, la);
        ir::IrInstr k = make_op(ir::IrOp::CONST);
        k.type = ir::IrType::I64;
        k.dst = ty;
        fn.append(entry, k);
        ir::IrInstr te = make_op(ir::IrOp::TRYENTER);
        te.operands = {pc, ty};
        te.target_block = handler;
        fn.append(entry, te);
        ir::IrInstr br = make_op(ir::IrOp::BR);
        br.target_block = body;
        fn.append(entry, br);
        fn.append(body, make_op(ir::IrOp::RET));
        fn.append(handler, make_op(ir::IrOp::RET));
        mod.functions.push_back(std::move(fn));
        std::vector<std::string> errors;
        const bool ok = ir::ir_verify(mod, errors);
        if (mismatch)
            CHECK(!ok && !errors.empty(),
                  "el verificador no acusa un tryenter con dos manejadores");
        else {
            CHECK(ok, "el verificador acusa un tryenter coherente");
            for (const std::string &e : errors)
                std::printf("    %s\n", e.c_str());
        }
    }
}

int main() {
    check_catch_is_reachable();
    check_verify_tryenter_label();
    Rng r{0x9E3779B97F4A7C15ull};
    for (int i = 0; i < 20000; ++i)
        check_function(random_function(r));
    std::printf("=== dom_facts: %d comprobaciones, %d fallos ===\n", g_checks,
                g_fail);
    return g_fail == 0 ? 0 : 1;
}
