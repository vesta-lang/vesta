/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/ssa_coalesce.cpp
 * @brief Implementacion del coalescing de PHI basado en SSA (ver
 *        ssa_coalesce.h).
 *
 * Construye liveness PRECISA sobre el IR (single-def -> sin el problema
 * multi-def del post-out-of-SSA) con el esquema de 2 posiciones por instr
 * (use=2*gi, def=2*gi+1) y coalesce las congruencias de PHI cuyos clusters
 * no interfieren.  Espeja @c build_intervals (MachineIR) adaptado al IR +
 * la semantica de PHI (dst def al entrar el bloque; args usados al final del
 * predecesor).
 */

#include "jit/ssa_coalesce.h"

#include "jit/interval.h" // LiveInterval (add_range, first_overlap_from)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

namespace jit {

namespace {

/// True si @p op es un binop ALU two-address (`dst = op0 OP op1` -> se
/// legaliza a `mov dst, op0; OP dst, op1`).  Para estos, dst NO puede
/// compartir registro con op1 (operands[1]): el `mov dst, op0` clobberearia
/// op1 antes de leerlo.
bool is_two_addr_ir(ir::IrOp op) noexcept {
    switch (op) {
    case ir::IrOp::ADD:
    case ir::IrOp::SUB:
    case ir::IrOp::MUL:
    case ir::IrOp::DIV:
    case ir::IrOp::MOD:
    case ir::IrOp::AND:
    case ir::IrOp::OR:
    case ir::IrOp::XOR:
    case ir::IrOp::SHL:
    case ir::IrOp::SHR:
    case ir::IrOp::SAR: return true;
    default: return false;
    }
}

} // namespace

std::vector<uint32_t> ssa_phi_coalesce_remap(const ir::IrFunction &fn) {
    const uint32_t NV = static_cast<uint32_t>(fn.values.size());
    const uint32_t NB = static_cast<uint32_t>(fn.blocks.size());
    std::vector<uint32_t> remap; // vacio = nada que coalescer
    if (NV == 0 || NB == 0) return remap;

    /* ---- 1) Posiciones lineales por instruccion (use=2gi, def=2gi+1) ----
     * Cada bloque vacio ocupa 1 slot (como build_intervals) para no colapsar
     * posiciones. */
    std::vector<uint32_t> first_gi(NB, 0), block_start(NB, 0), block_end(NB, 0);
    {
        uint32_t gi = 0;
        for (uint32_t b = 0; b < NB; ++b) {
            first_gi[b] = gi;
            block_start[b] = 2u * gi;
            const size_t n = fn.blocks[b].instrs.size();
            gi += static_cast<uint32_t>(n == 0 ? 1 : n);
            block_end[b] = 2u * gi;
        }
    }

    /* Helper: recorre los USOS (IrValueId) de una instr.  Los args de PHI NO
     * son usos en este bloque (se usan al final del predecesor). */
    auto each_use = [](const ir::IrInstr &in, auto &&fn_use) {
        if (in.op == ir::IrOp::PHI) return; // args gestionados aparte
        for (ir::IrValueId u : in.operands)
            if (u != ir::IR_NO_VALUE) fn_use(u);
        if (in.op == ir::IrOp::CALLIND && in.func_ptr != ir::IR_NO_VALUE)
            fn_use(in.func_ptr);
    };

    /* ---- 2) gen/kill por bloque (bitsets planos char) ---- */
    auto idx = [NV](uint32_t b, uint32_t v) { return size_t(b) * NV + v; };
    std::vector<char> gen(size_t(NB) * NV, 0), kill(size_t(NB) * NV, 0);
    /* phi_defs[b] y, por arista, los args: precomputar para el dataflow. */
    for (uint32_t b = 0; b < NB; ++b) {
        for (const ir::IrInstr &in : fn.blocks[b].instrs) {
            each_use(in, [&](ir::IrValueId u) {
                if (u < NV && !kill[idx(b, u)]) gen[idx(b, u)] = 1;
            });
            if (in.dst != ir::IR_NO_VALUE && in.dst < NV)
                kill[idx(b, in.dst)] = 1; // incluye phi dst
        }
    }

    /* ---- 3) Liveness dataflow PHI-aware a punto fijo ----
     * live_out[b] = U_succ [ (live_in[succ] - phi_defs[succ]) U
     *                        {args de phis de succ que vienen de b} ]
     * live_in[b]  = gen[b] U (live_out[b] - kill[b]). */
    std::vector<char> live_in(size_t(NB) * NV, 0), live_out(size_t(NB) * NV, 0);
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t bi = NB; bi-- > 0;) {
            const ir::IrBlock &blk = fn.blocks[bi];
            /* new_out */
            std::vector<char> nout(NV, 0);
            for (ir::IrBlockId s : blk.succs) {
                if (s >= NB) continue;
                const ir::IrBlock &sb = fn.blocks[s];
                /* live_in[s] menos los phi-defs de s */
                for (uint32_t v = 0; v < NV; ++v)
                    if (live_in[idx(s, v)]) nout[v] = 1;
                for (const ir::IrInstr &in : sb.instrs) {
                    if (in.op != ir::IrOp::PHI) continue;
                    if (in.dst < NV) nout[in.dst] = 0; // quitar phi-def
                }
                /* mas los args de phis de s que vienen de bi */
                for (const ir::IrInstr &in : sb.instrs) {
                    if (in.op != ir::IrOp::PHI) continue;
                    for (const ir::IrPhiArg &a : in.phi_args)
                        if (a.block == bi && a.value < NV) nout[a.value] = 1;
                }
            }
            for (uint32_t v = 0; v < NV; ++v) {
                if (nout[v] != live_out[idx(bi, v)]) {
                    live_out[idx(bi, v)] = nout[v];
                    changed = true;
                }
            }
            /* new_in = gen U (out - kill) */
            for (uint32_t v = 0; v < NV; ++v) {
                char ni = gen[idx(bi, v)] ||
                          (live_out[idx(bi, v)] && !kill[idx(bi, v)]);
                if (ni != live_in[idx(bi, v)]) {
                    live_in[idx(bi, v)] = ni;
                    changed = true;
                }
            }
        }
    }

    /* ---- 4) Construccion de rangos precisos por valor ---- */
    std::vector<LiveInterval> iv(NV);
    for (uint32_t v = 0; v < NV; ++v) iv[v].vreg = v;
    std::vector<uint32_t> b_first(NV, UINT32_MAX), b_first_def(NV, UINT32_MAX),
        b_last_use(NV, 0);
    std::vector<uint8_t> b_used(NV, 0), b_def(NV, 0);
    std::vector<uint32_t> touched;
    for (uint32_t b = 0; b < NB; ++b) {
        const ir::IrBlock &blk = fn.blocks[b];
        const uint32_t bstart = block_start[b], bend = block_end[b];
        for (uint32_t v : touched) {
            b_first[v] = UINT32_MAX;
            b_first_def[v] = UINT32_MAX;
            b_last_use[v] = 0;
            b_used[v] = 0;
            b_def[v] = 0;
        }
        touched.clear();
        auto mark = [&](uint32_t v) {
            if (b_first[v] == UINT32_MAX && b_first_def[v] == UINT32_MAX &&
                !b_used[v] && !b_def[v])
                touched.push_back(v);
        };
        const uint32_t base = first_gi[b];
        for (size_t j = 0; j < blk.instrs.size(); ++j) {
            const ir::IrInstr &in = blk.instrs[j];
            const uint32_t gi = base + static_cast<uint32_t>(j);
            const uint32_t use_pos = 2u * gi, def_pos = 2u * gi + 1u;
            each_use(in, [&](ir::IrValueId u) {
                if (u >= NV) return;
                mark(u);
                b_used[u] = 1;
                b_last_use[u] = use_pos;
                if (use_pos < b_first[u]) b_first[u] = use_pos;
                iv[u].uses.push_back(use_pos);
            });
            if (in.dst != ir::IR_NO_VALUE && in.dst < NV) {
                mark(in.dst);
                b_def[in.dst] = 1;
                if (def_pos < b_first_def[in.dst]) b_first_def[in.dst] = def_pos;
                if (def_pos < b_first[in.dst]) b_first[in.dst] = def_pos;
            }
        }
        /* Rango de cada valor relevante en este bloque. */
        for (uint32_t v = 0; v < NV; ++v) {
            const bool in_ = live_in[idx(b, v)] != 0;
            const bool out_ = live_out[idx(b, v)] != 0;
            const bool appears = (b_first[v] != UINT32_MAX);
            if (!in_ && !out_ && !appears) continue;
            const uint32_t start =
                in_ ? bstart : (appears ? b_first[v] : bstart);
            uint32_t end;
            if (out_)
                end = bend;
            else if (b_used[v])
                end = b_last_use[v] + 1u;
            else if (b_def[v])
                end = b_first_def[v] + 1u;
            else
                end = bend;
            iv[v].add_range(start, end);
        }
    }

    /* ---- 4b) Usos NO-phi de cada valor (para la regla de soundness) ----
     * Un phi_arg `s` solo es SEGURO de coalescer con su phi_dst si TODOS sus
     * usos son phi-args: entonces s "muere" en el borde del phi (copia paralela
     * en la arista) y NO puede estar vivo a la vez que el phi_dst -> cero
     * interferencia.  Si s tiene ALGUN uso NO-phi, coalescer puede crear una
     * interferencia real que el analisis de rangos con numeracion lineal NO
     * modela bien a traves de un back-edge de loop -- era la causa del hang de
     * state_machine (el phi de `rng` se coalescia con `rng_next`, que ademas se
     * usa en el shift que calcula `byte`).  Es el subset provably-sound clasico
     * (Chaitin/Briggs): coalescer solo copias phi cuyo origen no vive fuera del
     * phi.  each_use ya EXCLUYE los PHI, asi que esto marca exactamente los
     * usos no-phi. */
    std::vector<char> non_phi_used(NV, 0);
    for (const auto &blk : fn.blocks)
        for (const ir::IrInstr &in : blk.instrs)
            each_use(in, [&](ir::IrValueId u) {
                if (u < NV) non_phi_used[u] = 1;
            });

    /* ---- 5) Coalescing de congruencias de PHI ---- */
    std::vector<uint32_t> parent(NV);
    for (uint32_t v = 0; v < NV; ++v) parent[v] = v;
    std::function<uint32_t(uint32_t)> find = [&](uint32_t x) -> uint32_t {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    std::vector<LiveInterval> merged = iv; // clusters (valido en el rep)
    std::vector<std::vector<uint32_t>> members(NV);
    for (uint32_t v = 0; v < NV; ++v) members[v].push_back(v);

    /* Pares PROHIBIDOS 2-address: dst<->operands[1]. */
    /* Marca de phi-dsts: un valor definido por un PHI.  Coalescer un phi_dst
     * con un arg que es a su vez un phi_dst forma CADENAS de phis a traves de
     * bloques merge (dispatch con if/else anidado); la liveness de esas
     * cadenas cross-merge tiene sutilezas que producen miscompilaciones.
     * Conservador: NO coalescer si el arg es un phi_dst.  Se conserva el win
     * comun (loop-carried acc/contador, cuyos args son ADD/const, no phis). */
    std::vector<char> is_phi_dst(NV, 0);
    for (uint32_t b = 0; b < NB; ++b)
        for (const ir::IrInstr &in : fn.blocks[b].instrs)
            if (in.op == ir::IrOp::PHI && in.dst != ir::IR_NO_VALUE &&
                in.dst < NV)
                is_phi_dst[in.dst] = 1;

    std::vector<std::vector<uint32_t>> forbidden(NV);
    for (uint32_t b = 0; b < NB; ++b) {
        for (const ir::IrInstr &in : fn.blocks[b].instrs) {
            if (!is_two_addr_ir(in.op)) continue;
            if (in.dst == ir::IR_NO_VALUE || in.operands.size() < 2) continue;
            const ir::IrValueId d = in.dst, o1 = in.operands[1];
            if (d < NV && o1 < NV && d != o1) {
                forbidden[d].push_back(o1);
                forbidden[o1].push_back(d);
            }
        }
    }
    auto has_forbidden = [&](uint32_t ra, uint32_t rb) -> bool {
        const uint32_t small =
            members[ra].size() <= members[rb].size() ? ra : rb;
        const uint32_t other = (small == ra) ? rb : ra;
        for (uint32_t m : members[small])
            for (uint32_t f : forbidden[m])
                if (find(f) == other) return true;
        return false;
    };
    auto interfere = [&](uint32_t ra, uint32_t rb) -> bool {
        return merged[ra].first_overlap_from(merged[rb], 0) != UINT32_MAX;
    };

    /* Posiciones de CALL en el IR (use_pos).  Un valor coalescido que cruza
     * un call debe ir a callee-saved; la interaccion del cluster multi-def
     * con esa logica del allocator produce bugs sutiles -> conservador:
     * NO coalescer valores vivos a traves de un call.  Se pierde el win del
     * acumulador cross-call (raro), se conserva el del loop puro (comun). */
    std::vector<uint32_t> call_pos;
    for (uint32_t b = 0; b < NB; ++b) {
        const uint32_t base = first_gi[b];
        for (size_t j = 0; j < fn.blocks[b].instrs.size(); ++j) {
            const ir::IrInstr &in = fn.blocks[b].instrs[j];
            const ir::IrOp op = in.op;
            bool is_call = (op == ir::IrOp::CALL || op == ir::IrOp::CALLN ||
                            op == ir::IrOp::CALLIND || op == ir::IrOp::CALLVIRT ||
                            op == ir::IrOp::CALLITF || op == ir::IrOp::CALLM ||
                            op == ir::IrOp::TAILCALL ||
                            /* ops GC con slow-path call inline (clobbean
                             * caller-saved): deref/alloc/promote/etc. */
                            op == ir::IrOp::GC_DEREF_HOST ||
                            op == ir::IrOp::GC_ALLOC ||
                            op == ir::IrOp::GC_ALLOCP ||
                            op == ir::IrOp::NEWOBJ ||
                            op == ir::IrOp::GC_HANDLE_FOR_PTR ||
                            op == ir::IrOp::GC_PROMOTE ||
                            op == ir::IrOp::GC_DEMOTE);
            /* LOAD/STORE sobre memoria VM (puntero NO host) baja a LOAD_VM/
             * STORE_VM, que hace un CALL a vrt_vm_read/write en el page-miss
             * -> clobbea caller-saved.  Se detecta por is_host_ptr del ptr. */
            if (op == ir::IrOp::LOAD && !in.operands.empty()) {
                const ir::IrValueId p = in.operands[0];
                if (p < NV && !fn.values[p].is_host_ptr) is_call = true;
            }
            if (op == ir::IrOp::STORE && in.operands.size() >= 1) {
                const ir::IrValueId p = in.operands[0];
                if (p < NV && !fn.values[p].is_host_ptr) is_call = true;
            }
            if (is_call)
                call_pos.push_back(2u * (base + static_cast<uint32_t>(j)));
        }
    }
    /* Deteccion DIRECTA y robusta de cross-call por valor: un valor cruza un
     * call si (a) se usa en un bloque en una instr posterior a un call de ese
     * bloque (live a traves del call), o (b) es live-in a un bloque que
     * contiene un call y se usa/pasa mas alla.  Se computa por escaneo simple
     * (over-approxima -> conservador/seguro).  Cubre el patron
     * `acc_next = acc + callresult` de un dispatch polimorfico donde el
     * acumulador live-in cruza el callvirt de la rama. */
    std::vector<char> vcross(NV, 0);
    {
        /* Marcar los valores usados tras un call en su bloque. */
        for (uint32_t b = 0; b < NB; ++b) {
            const std::vector<ir::IrInstr> &ins = fn.blocks[b].instrs;
            bool seen_call = false;
            /* first_call_j = indice de la primera instr-call del bloque. */
            for (size_t j = 0; j < ins.size(); ++j) {
                const ir::IrOp op = ins[j].op;
                bool is_c =
                    (op == ir::IrOp::CALL || op == ir::IrOp::CALLN ||
                     op == ir::IrOp::CALLIND || op == ir::IrOp::CALLVIRT ||
                     op == ir::IrOp::CALLITF || op == ir::IrOp::CALLM ||
                     op == ir::IrOp::TAILCALL || op == ir::IrOp::GC_DEREF_HOST ||
                     op == ir::IrOp::GC_ALLOC || op == ir::IrOp::GC_ALLOCP ||
                     op == ir::IrOp::NEWOBJ ||
                     op == ir::IrOp::GC_HANDLE_FOR_PTR ||
                     op == ir::IrOp::GC_PROMOTE || op == ir::IrOp::GC_DEMOTE ||
                     /* ALLOCA modifica RSP; LOAD/STORE sobre memoria VM bajan a
                      * LOAD_VM/STORE_VM (CALL a vrt_vm_read/write en page-miss)
                      * -> clobbean caller-saved.  Conservador: cualquier
                      * ALLOCA/LOAD/STORE en el bloque cuenta como clobber
                      * (el is_host_ptr no siempre esta poblado a tiempo). */
                     op == ir::IrOp::ALLOCA || op == ir::IrOp::LOAD ||
                     op == ir::IrOp::STORE);
                if (seen_call) {
                    each_use(ins[j], [&](ir::IrValueId u) {
                        if (u < NV) vcross[u] = 1;
                    });
                }
                if (is_c) seen_call = true;
            }
            /* Si el bloque tiene un call, los valores LIVE-IN cruzan ese call
             * (viven desde antes del bloque, atraviesan el call). */
            if (seen_call) {
                for (uint32_t v = 0; v < NV; ++v)
                    if (live_in[idx(b, v)]) vcross[v] = 1;
            }
        }
    }
    auto crosses_call = [&](uint32_t rep) -> bool {
        for (uint32_t m : members[rep])
            if (m < NV && vcross[m]) return true;
        return false;
    };

    static const bool dbg = [] {
        const char *v = std::getenv("VESTA_SSA_COAL_DBG");
        return v && v[0] != '\0' && v[0] != '0';
    }();

    bool any = false;
    for (uint32_t b = 0; b < NB; ++b) {
        for (const ir::IrInstr &in : fn.blocks[b].instrs) {
            if (in.op != ir::IrOp::PHI || in.dst == ir::IR_NO_VALUE) continue;
            for (const ir::IrPhiArg &a : in.phi_args) {
                const ir::IrValueId d = in.dst, s = a.value;
                if (d >= NV || s >= NV || d == s) continue;
                if (iv[d].empty() || iv[s].empty()) continue;
                if (is_phi_dst[s]) continue; // no cadenas de phis (cross-merge)
                if (non_phi_used[s]) { // arg vive fuera del phi -> puede interferir
                    if (dbg)
                        std::fprintf(stderr,
                                     "[ssa-coal] %s phi v%u<-v%u NONPHIUSE\n",
                                     fn.name.c_str(), d, s);
                    continue;
                }
                const uint32_t rd = find(d), rs = find(s);
                if (rd == rs) continue;
                if (crosses_call(rd) || crosses_call(rs)) {
                    if (dbg)
                        std::fprintf(stderr,
                                     "[ssa-coal] %s phi v%u<-v%u CROSSCALL\n",
                                     fn.name.c_str(), d, s);
                    continue;
                }
                if (interfere(rd, rs) || has_forbidden(rd, rs)) {
                    if (dbg)
                        std::fprintf(stderr,
                                     "[ssa-coal] %s phi v%u<-v%u REJECT\n",
                                     fn.name.c_str(), d, s);
                    continue;
                }
                if (dbg)
                    std::fprintf(stderr, "[ssa-coal] %s phi v%u<-v%u OK\n",
                                 fn.name.c_str(), d, s);
                for (const LiveRange &r : merged[rs].ranges)
                    merged[rd].add_range(r.from, r.to);
                members[rd].insert(members[rd].end(), members[rs].begin(),
                                   members[rs].end());
                parent[rs] = rd;
                any = true;
            }
        }
    }

    if (!any) return remap; // vacio
    remap.resize(NV);
    for (uint32_t v = 0; v < NV; ++v) remap[v] = find(v);
    return remap;
}

bool apply_ssa_coalesce(MFunction &mf, const ir::IrFunction &fn) {
    /* Gate: OPT-IN (default OFF).  El coalescing sobre liveness de RANGOS
     * LINEALES casera es UNSOUND en dos clases distintas ya observadas en
     * state_machine (-m jit):
     *   1) phi de `rng` coalescido con `rng_next` (que vive fuera del phi) ->
     *      corrompe el valor -> BUCLE INFINITO.  Lo ataja la regla "el phi_arg
     *      solo se usa en phis" (seccion 4b).
     *   2) diamante `%d = phi[%s, %o]` con `%s = %o + c` (un arg DERIVADO del
     *      otro arg del MISMO phi): coalescer %d<-%s mientras %o vive rompe la
     *      copia del else -> RESULTADO INCORRECTO (counts 0x967e vs 0xb7d4).
     *      La regla single-use NO lo ataja (%s solo se usa en el phi).
     * La numeracion lineal no modela la interferencia de hermanos en el if/else
     * ni el back-edge del loop.  FIX REAL: construir un GRAFO DE INTERFERENCIA
     * de verdad sobre el SSA (o reusar build_intervals de interval.cpp) en vez
     * de la aproximacion de rangos.  Hasta entonces queda OFF (cero miscompile).
     * Activar (medir/desarrollar el fix) con VESTA_SSA_COALESCE=1; el
     * diff_harness caza VREG_HANG y DIVERGE, asi que el fix esta protegido. */
    static const bool on = [] {
        const char *en = std::getenv("VESTA_SSA_COALESCE");
        return en && en[0] != '\0' && en[0] != '0';
    }();
    if (!on) return false;
    const std::vector<uint32_t> remap = ssa_phi_coalesce_remap(fn);
    if (remap.empty()) return false;
    const uint32_t NVAL = static_cast<uint32_t>(remap.size());
    bool changed = false;
    auto rw = [&](MOperand &o) {
        if (!o.is_vreg()) return;
        const uint32_t vid = o.vreg_id();
        if (vid < NVAL && remap[vid] != vid) {
            o = MOperand::make_vreg(remap[vid], o.vreg_class(), o.width);
            changed = true;
        }
    };
    for (MBlock &b : mf.blocks) {
        for (MInstr &in : b.instrs) {
            rw(in.dst);
            rw(in.src1);
            rw(in.src2);
        }
    }
    /* Eliminar los phi-copies que quedaron self (`MOV vX, vX`) ANTES de
     * reconstruir intervalos: si se dejan, build_intervals ve un def+use de vX
     * al final del bloque (la copia), lo que puede alterar el rango
     * loop-carried y confundir la deteccion cross-call del allocator.  Borrar
     * la copia muerta deja el rango limpio.  (Los self-moves fisicos residuales
     * los pilla el peephole tras el rewrite.) */
    if (changed) {
        for (MBlock &b : mf.blocks) {
            std::vector<MInstr> kept;
            kept.reserve(b.instrs.size());
            for (const MInstr &in : b.instrs) {
                const bool self_move =
                    (in.op == MOp::MOV || in.op == MOp::MOVSD ||
                     in.op == MOp::MOVSS) &&
                    in.dst.is_vreg() && in.src1.is_vreg() &&
                    in.src2.kind == MOperandKind::NONE &&
                    in.dst.vreg_id() == in.src1.vreg_id();
                if (!self_move) kept.push_back(in);
            }
            if (kept.size() != b.instrs.size()) b.instrs = std::move(kept);
        }
    }
    return changed;
}

} // namespace jit
