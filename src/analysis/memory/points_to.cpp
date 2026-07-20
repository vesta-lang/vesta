/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file points_to.cpp
 * @brief Implementacion del resolvedor de direcciones compartido.  Unifica la
 *        resolucion (raiz + offset const) que antes vivia duplicada en el DSE
 *        (addr_of, byte-precisa) y en el modelo de efectos (classify_ptr,
 *        base-only).  Ahora hay UNA sola: precisa (offset const) + sound (nunca
 *        afirma un offset no probado).
 */
#include "analysis/memory/points_to.h"

#include "ir/ssa_ir.h"

namespace analysis {

using effects::AbstractLoc;
using K = effects::AbstractLoc::Kind;

namespace {

// Lee una constante entera de un valor SSA (para acumular offsets const).
bool const_val_of(const ir::IrFunction &fn, ir::IrValueId id, int64_t &out) {
    if (id == ir::IR_NO_VALUE ||
        id >= static_cast<ir::IrValueId>(fn.values.size()))
        return false;
    const ir::IrValue &v = fn.values[id];
    if (!v.is_const) return false;
    out = static_cast<int64_t>(v.const_val);
    return true;
}

// Estado de la resolucion (memoizacion + guardia de ciclos por PHI).
struct Resolver {
    const ir::IrFunction &fn;
    const IrFacts        &facts;
    std::vector<PointsToEntry> memo;
    std::vector<uint8_t>       state; // 0=nuevo, 1=en-curso, 2=hecho

    Resolver(const ir::IrFunction &f, const IrFacts &fc)
        : fn(f), facts(fc) {
        const size_t n = facts.def_of.size();
        memo.assign(n, PointsToEntry{});
        state.assign(n, 0);
    }

    // Raiz concreta: kind + root=self, offset 0, exacto.
    static PointsToEntry root_loc(K kind, ir::IrValueId self) {
        return PointsToEntry{kind, self, 0, true};
    }
    static PointsToEntry unknown() {
        return PointsToEntry{K::Unknown, effects::LOC_GENERIC, 0, false};
    }

    // Aplica un offset const a una entrada (respeta off_exact).
    static PointsToEntry with_offset(PointsToEntry base, int64_t delta) {
        if (base.kind == K::Unknown) return base;
        base.off += delta;
        return base; // off_exact heredado (se degrada abajo si el delta no era const)
    }

    // Degrada una entrada concreta a "raiz conocida, offset inexacto".
    static PointsToEntry inexact(PointsToEntry base) {
        if (base.kind == K::Unknown) return base;
        base.off = 0;
        base.off_exact = false;
        return base;
    }

    const PointsToEntry &resolve(ir::IrValueId v) {
        if (v == ir::IR_NO_VALUE ||
            v >= static_cast<ir::IrValueId>(memo.size())) {
            static const PointsToEntry kUnk = unknown();
            return kUnk;
        }
        if (state[v] == 2) return memo[v];
        if (state[v] == 1) { // ciclo (PHI): no se puede resolver -> Unknown
            memo[v] = unknown();
            state[v] = 2;
            return memo[v];
        }
        state[v] = 1;
        memo[v] = compute(v);
        state[v] = 2;
        return memo[v];
    }

    PointsToEntry compute(ir::IrValueId v) {
        // Parametro: memoria alcanzable desde el arg (points-to grueso).
        const int32_t pidx = facts.param_index(v);
        if (pidx >= 0)
            return PointsToEntry{K::ArgDerived, static_cast<uint32_t>(pidx), 0, true};

        const ir::IrInstr *d = facts.def(v);
        if (!d) return unknown();

        using Op = ir::IrOp;
        switch (d->op) {
        // --- Raices ---
        case Op::ALLOCA:
            return root_loc(K::Stack, v);
        case Op::RAW_ALLOC:
        case Op::GC_ALLOC:
        case Op::GC_ALLOCP:
        case Op::NEWOBJ:
        case Op::NEWOBJS:
        case Op::ARRAY_ALLOC:
        case Op::STRMAKE:
        case Op::STRRESERVE:
            return root_loc(K::Heap, v);
        case Op::GETSTATIC:
        case Op::STR_LIT_ADDR:
        case Op::LABEL_ADDR:
        case Op::SECTION_REF:
            return root_loc(K::Global, v);

        // --- Derivaciones que preservan raiz Y offset (misma direccion) ---
        case Op::MOV:
        case Op::BITCAST:
        case Op::CAST:
        case Op::GCDEREF_IR:
        case Op::GC_DEREF_HOST:
        case Op::GC_HANDLE_FOR_PTR:
        case Op::UNWRAP:
        case Op::MVTAKE_IR:
            if (!d->operands.empty()) return resolve(d->operands[0]);
            return unknown();

        // --- ADD con offset const: acumula ---
        case Op::ADD: {
            if (d->operands.size() != 2) return unknown();
            int64_t c;
            // base + const
            {
                PointsToEntry b = resolve(d->operands[0]);
                if (b.kind != K::Unknown && b.off_exact &&
                    const_val_of(fn, d->operands[1], c))
                    return with_offset(b, c);
            }
            // const + base
            {
                PointsToEntry b = resolve(d->operands[1]);
                if (b.kind != K::Unknown && b.off_exact &&
                    const_val_of(fn, d->operands[0], c))
                    return with_offset(b, c);
            }
            return unknown();
        }

        // --- GEP: misma raiz, offset NO probado (escala desconocida aqui) ---
        case Op::GEP:
            if (!d->operands.empty()) return inexact(resolve(d->operands[0]));
            return unknown();

        default:
            // Cargado de memoria, PHI, const-address, calculo arbitrario...
            return unknown();
        }
    }
};

} // namespace

PointsTo compute_points_to(const ir::IrFunction &fn, const IrFacts &facts) {
    Resolver r(fn, facts);
    PointsTo out;
    const size_t n = facts.def_of.size();
    out.loc.assign(n, PointsToEntry{});
    for (ir::IrValueId v = 0; v < static_cast<ir::IrValueId>(n); ++v)
        out.loc[v] = r.resolve(v);
    return out;
}

AbstractLoc loc_of(const PointsTo &pt, ir::IrValueId ptr, int32_t width) {
    const PointsToEntry &e = pt.at(ptr);
    if (e.kind == K::Unknown)
        return AbstractLoc{K::Unknown, effects::LOC_GENERIC, 0, 0};
    if (!e.off_exact) {
        // Raiz conocida pero offset no probado -> objeto entero (width 0): puede
        // solapar cualquier acceso a la misma raiz, disjunto de otras raices.
        return AbstractLoc{e.kind, e.root, 0, 0};
    }
    return AbstractLoc{e.kind, e.root, e.off, width};
}

} // namespace analysis
