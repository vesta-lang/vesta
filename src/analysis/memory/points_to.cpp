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
#include "analysis/memory/memory_access.h" // tamano de un tipo (UNICA verdad)

#include "ir/ssa_ir.h"

namespace analysis {

char PointsToAnalysis::ID = 0;

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
    const RangeFacts     *rangos = nullptr;
    std::vector<PointsToEntry> memo;
    std::vector<uint8_t>       state; // 0=nuevo, 1=en-curso, 2=hecho

    Resolver(const ir::IrFunction &f, const IrFacts &fc, const RangeFacts *rg)
        : fn(f), facts(fc), rangos(rg) {
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
    //
    // @p sym, si se da, es el valor que aporta el desplazamiento variable: no
    // se sabe CUANTO, pero si QUIEN, y con su rango se puede acotar despues.
    static PointsToEntry inexact(PointsToEntry base,
                                 ir::IrValueId sym = ir::IR_NO_VALUE) {
        if (base.kind == K::Unknown) return base;
        base.off = 0;
        base.off_exact = false;
        base.off_sym = sym;
        return base;
    }

    /**
     * @brief Como @c inexact, pero ACOTANDO el desplazamiento con su rango.
     *
     * `buf + i` no tiene offset constante; con el rango de `i` si tiene
     * INTERVALO, y un intervalo ya es una respuesta: dice entre que dos
     * posiciones cae el acceso.  Sin rangos se comporta igual que antes.
     *
     * Se suma el offset constante que ya llevara la base (`(buf+8) + i`), que
     * es lo que compone la geometria de una vista.
     */
    PointsToEntry con_rango(PointsToEntry base, ir::IrValueId sym) {
        const int64_t base_off = base.off;
        PointsToEntry e = inexact(base, sym);
        if (!rangos) return e;
        const ValueRange &r = rangos->at(sym);
        if (!r.conocido) return e;
        // Con freno: si la suma se desborda, no se afirma el intervalo.
        const int64_t lo = base_off + r.lo, hi = base_off + r.hi;
        if ((r.lo > 0 && lo < base_off) || (r.hi > 0 && hi < base_off))
            return e;
        e.off_lo = lo;
        e.off_hi = hi;
        e.off_rango = true;
        return e;
    }

    /// PHI que se esta resolviendo ahora mismo.  Volver a ella por un arg no es
    /// "no se sabe": es el propio bucle, y ese arg simplemente no aporta.
    ir::IrValueId phi_en_curso = ir::IR_NO_VALUE;

    const PointsToEntry &resolve(ir::IrValueId v) {
        if (v == ir::IR_NO_VALUE ||
            v >= static_cast<ir::IrValueId>(memo.size())) {
            static const PointsToEntry kUnk = unknown();
            return kUnk;
        }
        if (state[v] == 2) return memo[v];
        if (state[v] == 1) {
            /* Ciclo.  Si es la PHI que estamos resolviendo, se devuelve un
             * "no aporta" (None) SIN memoizar: memoizarlo la dejaria marcada
             * como desconocida para siempre, que es lo que hacia que un puntero
             * inducido -- `q = p; q += 1` en cada vuelta, en lo que el
             * optimizador convierte `p + i` -- perdiera su raiz y con ella toda
             * posibilidad de comprobar el acceso.  Un bucle indexado es justo
             * donde se desborda un buffer. */
            if (v == phi_en_curso) {
                static const PointsToEntry kNoAporta{K::None, effects::LOC_GENERIC,
                                                     0, false};
                return kNoAporta;
            }
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
            /* Desplazamiento NO constante (`buf + i*8`: indexar con una
             * variable).  Se conserva la RAIZ y se marca el offset como no
             * probado -- exactamente lo que hace @c GEP justo debajo.
             *
             * Antes se devolvia "desconocido", que tira la raiz, y con ella
             * toda posibilidad de distinguir dos objetos DISTINTOS.  Como
             * recorrer un buffer con un indice es el caso normal de cualquier
             * bucle, eso dejaba ciega la desambiguacion justo donde mas falta
             * hace: una escritura en un array volvia opaco el bucle entero.
             *
             * Solo cuando UNO de los dos lados tiene raiz conocida: si la
             * tienen los dos (sumar dos punteros) no hay forma de decir cual
             * manda, y si no la tiene ninguno no hay nada que conservar. */
            {
                const PointsToEntry a = resolve(d->operands[0]);
                const PointsToEntry b = resolve(d->operands[1]);
                const bool a_raiz = a.kind != K::Unknown && a.kind != K::None;
                const bool b_raiz = b.kind != K::Unknown && b.kind != K::None;
                if (a_raiz && !b_raiz) return con_rango(a, d->operands[1]);
                if (b_raiz && !a_raiz) return con_rango(b, d->operands[0]);
            }
            return unknown();
        }

        // --- GEP: misma raiz, offset NO probado (escala desconocida aqui) ---
        case Op::GEP:
            if (!d->operands.empty()) return inexact(resolve(d->operands[0]));
            return unknown();

        /* PHI: si todos los caminos que APORTAN traen la misma raiz, la raiz se
         * conserva.  El desplazamiento no -- por eso queda inexacto --, pero la
         * raiz es lo que permite saber DE QUE objeto se habla, y sin ella no hay
         * nada que comprobar.
         *
         * Es el caso de un puntero inducido: `q = buf` al entrar y `q = q + 1`
         * al volver.  El segundo camino vuelve a la propia PHI y no aporta; el
         * primero dice `buf`.  Antes se devolvia "desconocido" y un bucle que
         * recorre un buffer quedaba fuera de todo analisis.
         *
         * Si dos caminos traen raices DISTINTAS no se afirma nada: no hay forma
         * de decir cual manda. */
        case Op::PHI: {
            if (d->operands.empty()) return unknown();
            const ir::IrValueId marca_previa = phi_en_curso;
            phi_en_curso = v;
            PointsToEntry acc;
            bool primero = true, discrepan = false;
            for (ir::IrValueId a : d->operands) {
                const PointsToEntry e = resolve(a);
                if (e.kind == K::None) continue;      // el propio bucle
                if (e.kind == K::Unknown) { discrepan = true; break; }
                if (primero) { acc = e; primero = false; continue; }
                if (e.kind != acc.kind || e.root != acc.root) {
                    discrepan = true;
                    break;
                }
            }
            phi_en_curso = marca_previa;
            if (discrepan || primero) return unknown();
            return inexact(acc);
        }

        default:
            // Cargado de memoria, const-address, calculo arbitrario...
            return unknown();
        }
    }
};

} // namespace

/**
 * @brief Extension de la region que crea una instruccion de asignacion.
 *
 * El tamano esta escrito en el sitio de asignacion; aqui solo se lee y se dice
 * con que grado de certeza se sabe.  `alloca.T count` da count * sizeof(T);
 * `raw_alloc %size` da el operando, constante si lo es y simbolico si no --
 * simbolico NO es desconocido: se sabe QUE valor manda, que es lo que permitira
 * acotarlo con un rango.
 */
static RegionExtent extension_de(const ir::IrInstr &d, const IrFacts &facts) {
    RegionExtent ex;
    using Op = ir::IrOp;
    // Redondeo al hueco: un objeto ocupa su slot, no su tamano.  La pila
    // alinea a 8; el allocador del monton, a 16.  Es el limite que se puede
    // AFIRMAR -- por debajo de el, ensanchar un acceso es legitimo.
    auto redondear = [](int64_t n, int64_t a) {
        return n <= 0 ? n : ((n + a - 1) / a) * a;
    };
    if (d.op == Op::ALLOCA) {
        const int32_t elem = analysis::memory_access_size(d.type);
        const int64_t n = static_cast<int64_t>(d.imm);
        if (elem > 0 && n >= 0) {
            ex.bytes = n * elem;
            ex.reservado = redondear(ex.bytes, 8);
        }
        return ex;
    }
    if (d.op == Op::RAW_ALLOC || d.op == Op::GC_ALLOC || d.op == Op::GC_ALLOCP) {
        if (d.operands.empty()) return ex;
        const ir::IrValueId vs = d.operands[0];
        const ir::IrInstr *ds = facts.def(vs);
        if (ds && ds->op == Op::CONST) {
            ex.bytes = static_cast<int64_t>(ds->imm);
            ex.reservado = redondear(ex.bytes, 16);
        } else {
            ex.sym = vs;
        }
    }
    return ex;
}

PointsTo compute_points_to(const ir::IrFunction &fn, const IrFacts &facts,
                           const RangeFacts *rangos) {
    Resolver r(fn, facts, rangos);
    PointsTo out;
    const size_t n = facts.def_of.size();
    out.loc.assign(n, PointsToEntry{});
    out.extent.assign(n, RegionExtent{});
    for (ir::IrValueId v = 0; v < static_cast<ir::IrValueId>(n); ++v) {
        out.loc[v] = r.resolve(v);
        // La extension se guarda en la RAIZ, que es de quien es propiedad.
        if (const ir::IrInstr *d = facts.def(v))
            out.extent[v] = extension_de(*d, facts);
    }
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

ir::IrValueId valor_unico_del_hueco(const ir::IrFunction &fn,
                                    ir::IrValueId slot) {
    if (slot == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
    ir::IrValueId guardado = ir::IR_NO_VALUE;
    for (const ir::IrBlock &bb : fn.blocks) {
        for (const ir::IrInstr &in : bb.instrs) {
            if (in.op != ir::IrOp::STORE || in.operands.size() < 2) continue;
            if (in.operands[1] != slot) continue;
            // Una segunda escritura: el contenido ya depende del camino.
            if (guardado != ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            guardado = in.operands[0];
        }
    }
    return guardado;
}

} // namespace analysis
