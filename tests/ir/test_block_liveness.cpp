/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/ir/test_block_liveness.cpp
 * @brief Los vivos por bloque sin punto fijo dan los MISMOS conjuntos que con.
 *
 * @c compute_block_liveness sustituye al punto fijo sobre bitsets que tenia
 * dentro el coalescing de phis, y que era cuadratico en el tamano de la
 * funcion.  Que sea mas rapido no vale nada si responde otra cosa, asi que el
 * oraculo de este fichero es ese punto fijo, copiado tal cual -- con DOS
 * cambios, los dos tambien en el productor: el @c func_ptr cuenta como uso en
 * cualquier operacion, no solo en @c CALLIND, y los destinos de phi se restan
 * de la fila de SU sucesor, no del acumulado (ver el comentario en el bucle).
 *
 * Se compara sobre miles de funciones AL AZAR, no sobre un punado escrito a
 * mano: grafos con bucles, autolazos, bloques inalcanzables, phis cuyo
 * argumento nombra un bloque que no salta al suyo, usos antes de la definicion
 * y valores definidos en mas de un bloque.  Lo ultimo no es SSA, y esta a
 * proposito: el productor promete el mismo resultado que el punto fijo tambien
 * ahi.
 */
#include "ir/block_liveness.h"
#include "ir/ssa_ir.h"

#include <cstdint>
#include <cstdio>
#include <vector>

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
//  Oraculo: el punto fijo de `ssa_phi_coalesce_remap`, tal cual.
// --------------------------------------------------------------------------

/// Resultado del oraculo: una fila de bits por bloque, entrada y salida.
struct OracleLiveness {
    size_t words = 0;
    std::vector<uint64_t> live_in, live_out;

    bool in(uint32_t b, uint32_t v) const {
        return (live_in[b * words + (v >> 6)] >> (v & 63)) & 1ull;
    }
    bool out(uint32_t b, uint32_t v) const {
        return (live_out[b * words + (v >> 6)] >> (v & 63)) & 1ull;
    }
};

static void set_bit(uint64_t *r, uint32_t v) { r[v >> 6] |= 1ull << (v & 63); }
static void clr_bit(uint64_t *r, uint32_t v) {
    r[v >> 6] &= ~(1ull << (v & 63));
}
static bool get_bit(const uint64_t *r, uint32_t v) {
    return (r[v >> 6] >> (v & 63)) & 1ull;
}

/// Anota en gen/kill los usos de una instruccion que no sea PHI.
static void oracle_uses(const ir::IrInstr &in, uint32_t nv, uint64_t *g,
                        const uint64_t *k) {
    if (in.op == ir::IrOp::PHI) return; // args gestionados aparte
    for (ir::IrValueId u : in.operands)
        if (u < nv && !get_bit(k, u)) set_bit(g, u);
    if (in.func_ptr < nv && !get_bit(k, in.func_ptr)) set_bit(g, in.func_ptr);
}

static OracleLiveness oracle(const ir::IrFunction &fn) {
    const uint32_t NV = static_cast<uint32_t>(fn.values.size());
    const uint32_t NB = static_cast<uint32_t>(fn.blocks.size());
    OracleLiveness r;
    const size_t W = (size_t(NV) + 63) / 64;
    r.words = W;
    std::vector<uint64_t> gen(size_t(NB) * W, 0), kill(size_t(NB) * W, 0);
    for (uint32_t b = 0; b < NB; ++b) {
        uint64_t *g = gen.data() + size_t(b) * W;
        uint64_t *k = kill.data() + size_t(b) * W;
        for (const ir::IrInstr &in : fn.blocks[b].instrs) {
            oracle_uses(in, NV, g, k);
            if (in.dst != ir::IR_NO_VALUE && in.dst < NV)
                set_bit(k, in.dst); // incluye phi dst
        }
    }
    r.live_in.assign(size_t(NB) * W, 0);
    r.live_out.assign(size_t(NB) * W, 0);
    bool changed = true;
    std::vector<uint64_t> nout(W, 0), tmp(W, 0);
    while (changed) {
        changed = false;
        for (uint32_t bi = NB; bi-- > 0;) {
            const ir::IrBlock &blk = fn.blocks[bi];
            std::fill(nout.begin(), nout.end(), 0);
            for (ir::IrBlockId s : blk.succs) {
                if (s >= NB) continue;
                const ir::IrBlock &sb = fn.blocks[s];
                /* live_in[s] menos los phi-defs de s, restados sobre la fila
                 * de ESTE sucesor.  El original los borraba del acumulado, y
                 * con eso tambien lo que habia aportado otro sucesor: el
                 * resultado dependia del orden de `succs`.  Solo se nota en un
                 * IR que no es SSA estricto -- ahi un destino de phi de un
                 * sucesor puede vivir a la entrada de otro --, que es
                 * justamente lo que el generador de abajo produce. */
                const uint64_t *lis = r.live_in.data() + size_t(s) * W;
                std::copy(lis, lis + W, tmp.begin());
                for (const ir::IrInstr &in : sb.instrs) {
                    if (in.op != ir::IrOp::PHI) continue;
                    if (in.dst < NV) clr_bit(tmp.data(), in.dst);
                }
                for (size_t w = 0; w < W; ++w)
                    nout[w] |= tmp[w];
                for (const ir::IrInstr &in : sb.instrs) {
                    if (in.op != ir::IrOp::PHI) continue;
                    for (const ir::IrPhiArg &a : in.phi_args)
                        if (a.block == bi && a.value < NV)
                            set_bit(nout.data(), a.value);
                }
            }
            uint64_t *lo = r.live_out.data() + size_t(bi) * W;
            for (size_t w = 0; w < W; ++w)
                if (nout[w] != lo[w]) {
                    lo[w] = nout[w];
                    changed = true;
                }
            const uint64_t *g = gen.data() + size_t(bi) * W;
            const uint64_t *k = kill.data() + size_t(bi) * W;
            uint64_t *li = r.live_in.data() + size_t(bi) * W;
            for (size_t w = 0; w < W; ++w) {
                const uint64_t ni = g[w] | (lo[w] & ~k[w]);
                if (ni != li[w]) {
                    li[w] = ni;
                    changed = true;
                }
            }
        }
    }
    return r;
}

// --------------------------------------------------------------------------
//  Generador de funciones al azar (congruencial: reproducible).
// --------------------------------------------------------------------------

/// Generador pseudoaleatorio con estado propio, para que cada semilla de lo
/// mismo en cualquier maquina.
struct Rng {
    uint64_t state;
    uint32_t next(uint32_t bound) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>((state >> 33) % bound);
    }
};

static ir::IrFunction random_function(Rng &rng) {
    ir::IrFunction fn;
    const uint32_t nb = 1 + rng.next(14);
    const uint32_t nv = 1 + rng.next(40);
    for (uint32_t v = 0; v < nv; ++v)
        fn.new_value(ir::IrType::I64);
    for (uint32_t b = 0; b < nb; ++b)
        fn.new_block();
    for (uint32_t b = 0; b < nb; ++b) {
        ir::IrBlock &blk = fn.blocks[b];
        const uint32_t nsucc = rng.next(3); // 0, 1 o 2, con autolazos
        for (uint32_t s = 0; s < nsucc; ++s)
            blk.succs.push_back(ir::IrBlockId(rng.next(nb)));
        const uint32_t nphi = rng.next(3);
        for (uint32_t p = 0; p < nphi; ++p) {
            ir::IrInstr in;
            in.op = ir::IrOp::PHI;
            in.dst = ir::IrValueId(rng.next(nv));
            const uint32_t nargs = 1 + rng.next(3);
            for (uint32_t a = 0; a < nargs; ++a) // a veces de quien no salta
                in.phi_args.push_back(ir::IrPhiArg{
                    ir::IrValueId(rng.next(nv)), ir::IrBlockId(rng.next(nb))});
            blk.instrs.push_back(in);
        }
        const uint32_t nins = rng.next(7);
        for (uint32_t i = 0; i < nins; ++i) {
            ir::IrInstr in;
            const bool closure = rng.next(6) == 0;
            in.op = closure ? ir::IrOp::CALLCLOSURE : ir::IrOp::ADD;
            const uint32_t nops = rng.next(3);
            for (uint32_t o = 0; o < nops; ++o)
                in.operands.push_back(ir::IrValueId(rng.next(nv)));
            if (closure) in.func_ptr = ir::IrValueId(rng.next(nv));
            /* Sin destino una de cada cuatro; y como el destino sale al azar,
             * hay valores que se definen en mas de un bloque. */
            in.dst = rng.next(4) == 0 ? ir::IR_NO_VALUE
                                      : ir::IrValueId(rng.next(nv));
            blk.instrs.push_back(in);
        }
    }
    return fn;
}

// --------------------------------------------------------------------------
//  Comparacion.
// --------------------------------------------------------------------------

/// Comprueba que la fila del productor tiene exactamente los bits del oraculo
/// y que sale ordenada y sin repetidos.
static bool same_row(ir::IrValueList row, const OracleLiveness &o, uint32_t b,
                     uint32_t nv, bool outside) {
    uint32_t count = 0;
    for (uint32_t v = 0; v < nv; ++v)
        if (outside ? o.out(b, v) : o.in(b, v)) ++count;
    if (row.size() != count) return false;
    for (size_t i = 0; i < row.size(); ++i) {
        if (i > 0 && row[i - 1] >= row[i]) return false; // orden estricto
        if (!(outside ? o.out(b, row[i]) : o.in(b, row[i]))) return false;
    }
    return true;
}

/// Dice si @p v es el destino de un phi del bloque @p s.
static bool is_phi_dst_of(const ir::IrFunction &fn, ir::IrBlockId s,
                          uint32_t v) {
    for (const ir::IrInstr &in : fn.blocks[s].instrs)
        if (in.op == ir::IrOp::PHI && in.dst == v) return true;
    return false;
}

/// Cuenta que valores difieren en un bloque y si alguno es destino de un phi
/// de un sucesor, que es lo unico en que el oraculo depende del ORDEN.
static void report_difference(const ir::IrFunction &fn,
                              const ir::BlockLiveness &live,
                              const OracleLiveness &o, uint32_t b,
                              bool entrance) {
    const uint32_t nv = static_cast<uint32_t>(fn.values.size());
    std::vector<char> mine(nv, 0);
    const ir::IrValueList row = entrance ? live.live_in(ir::IrBlockId(b))
                                         : live.live_out(ir::IrBlockId(b));
    for (ir::IrValueId v : row)
        mine[v] = 1;
    for (uint32_t v = 0; v < nv; ++v) {
        const bool theirs = entrance ? o.in(b, v) : o.out(b, v);
        if (theirs == (mine[v] != 0)) continue;
        bool phi_dst_of_succ = false;
        for (ir::IrBlockId s : fn.blocks[b].succs)
            if (is_phi_dst_of(fn, s, v)) phi_dst_of_succ = true;
        std::printf("    v%u: oraculo=%d productor=%d  phi-dst de sucesor=%d\n",
                    v, theirs ? 1 : 0, mine[v], phi_dst_of_succ ? 1 : 0);
    }
}

static void test_random_functions() {
    Rng rng{0x5eed1234abcdull};
    int mismatches = 0;
    for (int n = 0; n < 20000; ++n) {
        const ir::IrFunction fn = random_function(rng);
        const OracleLiveness o = oracle(fn);
        const ir::BlockLiveness live = ir::compute_block_liveness(fn);
        const uint32_t nb = static_cast<uint32_t>(fn.blocks.size());
        const uint32_t nv = static_cast<uint32_t>(fn.values.size());
        for (uint32_t b = 0; b < nb; ++b) {
            const bool in_ok =
                same_row(live.live_in(ir::IrBlockId(b)), o, b, nv, false);
            const bool out_ok =
                same_row(live.live_out(ir::IrBlockId(b)), o, b, nv, true);
            if ((!in_ok || !out_ok) && mismatches < 5) {
                std::printf("  funcion %d, bloque %u: %s distinto\n", n, b,
                            in_ok ? "salida" : "entrada");
                report_difference(fn, live, o, b, !in_ok);
            }
            if (!in_ok || !out_ok) ++mismatches;
        }
    }
    CHECK(mismatches == 0, "los vivos por bloque difieren del punto fijo");
}

/// Un bucle de libro: el contador del phi vive alrededor de la arista de
/// retorno y el valor de antes del bucle vive a traves de el.
static void test_loop_by_hand() {
    ir::IrFunction fn;
    const ir::IrValueId init = fn.new_value(ir::IrType::I64);
    const ir::IrValueId i = fn.new_value(ir::IrType::I64);
    const ir::IrValueId next = fn.new_value(ir::IrType::I64);
    const ir::IrValueId outer = fn.new_value(ir::IrType::I64);
    const ir::IrBlockId entry = fn.new_block();
    const ir::IrBlockId head = fn.new_block();
    const ir::IrBlockId body = fn.new_block();
    const ir::IrBlockId exit = fn.new_block();
    fn.blocks[entry].succs = {head};
    fn.blocks[head].succs = {body, exit};
    fn.blocks[body].succs = {head};
    ir::IrInstr def_init;
    def_init.op = ir::IrOp::ADD;
    def_init.dst = init;
    fn.blocks[entry].instrs.push_back(def_init);
    ir::IrInstr def_outer = def_init;
    def_outer.dst = outer;
    fn.blocks[entry].instrs.push_back(def_outer);
    ir::IrInstr phi;
    phi.op = ir::IrOp::PHI;
    phi.dst = i;
    phi.phi_args = {ir::IrPhiArg{init, entry}, ir::IrPhiArg{next, body}};
    fn.blocks[head].instrs.push_back(phi);
    ir::IrInstr step;
    step.op = ir::IrOp::ADD;
    step.dst = next;
    step.operands.push_back(i);
    fn.blocks[body].instrs.push_back(step);
    ir::IrInstr use_outer;
    use_outer.op = ir::IrOp::ADD;
    use_outer.operands.push_back(outer);
    fn.blocks[exit].instrs.push_back(use_outer);

    const ir::BlockLiveness live = ir::compute_block_liveness(fn);
    const ir::IrValueList out_entry = live.live_out(entry);
    CHECK(out_entry.size() == 2 && out_entry[0] == init &&
              out_entry[1] == outer,
          "a la salida de la entrada viven el inicial y el de fuera");
    const ir::IrValueList in_body = live.live_in(body);
    CHECK(in_body.size() == 2 && in_body[0] == i && in_body[1] == outer,
          "el cuerpo recibe el contador y el valor de fuera");
    const ir::IrValueList out_body = live.live_out(body);
    CHECK(out_body.size() == 2 && out_body[0] == next && out_body[1] == outer,
          "el cuerpo entrega el siguiente, no el contador");
    CHECK(live.live_in(head).size() == 1 && live.live_in(head)[0] == outer,
          "el destino del phi no vive a la entrada de su bloque");
}

int main() {
    test_loop_by_hand();
    test_random_functions();
    std::printf("%d comprobaciones, %d fallos\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
