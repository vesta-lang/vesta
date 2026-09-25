/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file loop_metrics.cpp
 * @brief Implementacion del medidor NEUTRAL del cuerpo de un bucle.
 */

#include "analysis/facts/loop_metrics.h"
#include "analysis/memory/memory_access.h" // quien decide si una op toca memoria
#include "ir/ir_vec_ops.h" // cuales son las operaciones vectoriales

#include <unordered_set>

namespace analysis {

using ir::IR_NO_VALUE;
using ir::IrBlockId;
using ir::IrInstr;
using ir::IrOp;
using ir::IrValueId;

namespace {

/* Si una op lee o escribe memoria lo contesta el vocabulario de acceso, no una
 * lista propia.  Aqui habia dos, y no coincidian con las del optimizador: a
 * estas les faltaban ARRAY_LEN, VEC_ACC_ZERO y VEC_ACC_COMBINE, de modo que un
 * bucle que solo hiciera eso se contaba como si no tocara memoria.
 *
 * Es la clase de acceso EN EL IR: a donde baje cada op depende del objetivo, y
 * eso lo cubre la capa por backend, no este contador. */
bool is_load_like(IrOp op) {
    return analysis::memory_access_kind(op).is_load;
}
bool is_store_like(IrOp op) {
    return analysis::memory_access_kind(op).is_store;
}
bool is_call_like(IrOp op) {
    switch (op) {
    case IrOp::CALL:
    case IrOp::CALLN:
    case IrOp::CALLIND:
    case IrOp::CALLVIRT:
    case IrOp::CALLM:
    case IrOp::CALLITF:
    case IrOp::CALLCLOSURE:
    case IrOp::CALLSUPER:
    case IrOp::TAILCALL: return true;
    default: return false;
    }
}
bool is_vec(IrOp op) {
    return ir::is_vec_op(op);
}
bool is_fp(IrOp op) {
    switch (op) {
    case IrOp::FADD:
    case IrOp::FSUB:
    case IrOp::FMUL:
    case IrOp::FDIV:
    case IrOp::FNEG:
    case IrOp::FABS:
    case IrOp::FSQRT:
    case IrOp::FMIN:
    case IrOp::FMAX:
    case IrOp::FMA: return true;
    default: return false;
    }
}
bool is_expensive(IrOp op) {
    switch (op) {
    case IrOp::DIV:
    case IrOp::MOD:
    case IrOp::FDIV:
    case IrOp::FSQRT: return true;
    default: return false;
    }
}

} // namespace

namespace {

/// Un bucle de la lista que se mide.
enum MeasuredLoop : uint32_t {};
/// Ningun bucle medido.
constexpr MeasuredLoop NO_MEASURED_LOOP = MeasuredLoop(0xFFFFFFFFu);
/// Si un valor ya se conto como vivo fuera de su cuerpo.
enum LiveSeen : uint8_t { LIVE_NOT_SEEN = 0, LIVE_SEEN = 1 };

} // namespace

util::NamedVector<LiveAcross, scratch::LiveAcrossCounts> compute_live_across(
    const ir::IrFunction &fn,
    const std::vector<const std::vector<IrBlockId> *> &bodies) {
    const size_t nb = fn.blocks.size();
    const size_t nv = fn.values.size();
    util::NamedVector<LiveAcross, scratch::LiveAcrossCounts> count(
        bodies.size(), LiveAcross(0));
    /* De que cuerpo es cada bloque, y en que cuerpo se define cada valor. */
    util::NamedVector<MeasuredLoop, scratch::LiveAcrossLoopOf> loop_of(
        nb, NO_MEASURED_LOOP);
    for (size_t k = 0; k < bodies.size(); ++k)
        for (IrBlockId b : *bodies[k])
            if (b < nb) loop_of[b] = MeasuredLoop(k);
    util::NamedVector<MeasuredLoop, scratch::LiveAcrossDefIn> def_in(
        nv, NO_MEASURED_LOOP);
    for (size_t b = 0; b < nb; ++b) {
        if (loop_of[b] == NO_MEASURED_LOOP) continue;
        for (const IrInstr &in : fn.blocks[b].instrs)
            if (in.dst < nv) def_in[in.dst] = loop_of[b];
    }
    /* Cada uso una vez: si el valor es de un cuerpo y el uso esta fuera de
     * el, ese valor esta vivo a traves del cuerpo.  Cada valor cuenta una sola
     * vez, lo usen fuera cuantas instrucciones lo usen. */
    util::NamedVector<LiveSeen, scratch::LiveAcrossSeen> seen(nv,
                                                              LIVE_NOT_SEEN);
    for (size_t b = 0; b < nb; ++b) {
        const MeasuredLoop here = loop_of[b];
        for (const IrInstr &in : fn.blocks[b].instrs) {
            const size_t n_ops = in.operands.size();
            for (size_t j = 0; j < n_ops + in.phi_args.size(); ++j) {
                const IrValueId v = j < n_ops ? in.operands[j]
                                              : in.phi_args[j - n_ops].value;
                if (v >= nv || def_in[v] == NO_MEASURED_LOOP) continue;
                if (def_in[v] == here || seen[v] == LIVE_SEEN) continue;
                seen[v] = LIVE_SEEN;
                count[def_in[v]] = LiveAcross(count[def_in[v]] + 1);
            }
        }
    }
    return count;
}

LoopMetrics compute_loop_metrics(const ir::IrFunction &fn,
                                 const std::vector<IrBlockId> &body,
                                 LiveAcross live_across) {
    LoopMetrics m;
    for (IrBlockId b : body) {
        if (b >= fn.blocks.size()) continue;
        ++m.basic_blocks;
        for (const IrInstr &in : fn.blocks[b].instrs) {
            const bool is_term = (in.op == IrOp::BR || in.op == IrOp::BR_COND ||
                                  in.op == IrOp::RET || in.op == IrOp::THROW ||
                                  in.op == IrOp::UNREACHABLE);
            if (is_term) ++m.terminators;
            if (in.op == IrOp::PHI) {
                ++m.phis;
            } else if (!is_term) {
                ++m.instructions;
                if (is_load_like(in.op)) ++m.loads;
                if (is_store_like(in.op)) {
                    ++m.stores;
                    m.has_side_effects = true;
                }
                if (is_call_like(in.op)) {
                    ++m.calls;
                    m.has_side_effects = true;
                }
                if (is_vec(in.op)) ++m.vector_ops;
                if (is_fp(in.op)) ++m.fp_ops;
                if (is_expensive(in.op)) ++m.expensive_ops;
                // Efectos no capturados por los conteos: atomics, io, barreras.
                if (in.op == IrOp::ATOMIC_LD || in.op == IrOp::ATOMIC_ST ||
                    in.op == IrOp::ATOMIC_CAS || in.op == IrOp::ATOMIC_ADD ||
                    in.op == IrOp::RAW_ASM)
                    m.has_side_effects = true;
            }
            if (in.op == IrOp::BR_COND) ++m.branches;
        }
    }

    // Presion de registros (proxy): valores DEFINIDOS en el cuerpo que se usan
    // FUERA del cuerpo -- los back-args de las PHIs del header (loop-carried)
    // mas los live-out.  Son los unicos que cada copia del unroll mantiene
    // vivos a la vez; los temporales intra-iteracion se consumen dentro de su
    // copia y NO cuentan.  (El proxy anterior miraba el latch: en un bucle de
    // un solo bloque latch == cuerpo, contaba los temporales intra-iteracion e
    // inflaba la presion, capando el factor.)  Se calcula fuera, para todos los
    // bucles a la vez: ver @ref compute_live_across.
    m.live_across = static_cast<int>(live_across);
    return m;
}

} // namespace analysis
