/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file value_range.cpp
 * @brief Implementacion de los rangos de valor (ver value_range.h).
 *
 * Solo IR: ni un solo dato que dependa de la maquina, asi que el mismo hecho
 * vale para x86-64, x86-32 y arm64.
 */
#include "analysis/facts/value_range.h"

#include "analysis/facts/loop_facts.h"
#include "analysis/facts/loop_iv.h"
#include "analysis/facts/loop_structure.h"
#include "ir/ssa_ir.h"

#include <algorithm>

namespace analysis {

char RangeAnalysis::ID = 0;

namespace {

using ir::IrOp;
using ir::IrType;

/// Rango que impone el ANCHO del tipo.  Un `u8` no pasa de 255 en ninguna
/// arquitectura; es el suelo de conocimiento y sale gratis.
ValueRange del_tipo(IrType t) {
    ValueRange v;
    v.conocido = true;
    switch (t) {
    case IrType::I8: v.lo = -128; v.hi = 127; break;
    case IrType::I16: v.lo = -32768; v.hi = 32767; break;
    case IrType::I32: v.lo = INT32_MIN; v.hi = INT32_MAX; break;
    case IrType::U8: v.lo = 0; v.hi = 255; break;
    case IrType::U16: v.lo = 0; v.hi = 65535; break;
    case IrType::U32: v.lo = 0; v.hi = UINT32_MAX; break;
    case IrType::BOOL: v.lo = 0; v.hi = 1; break;
    case IrType::HANDLE: v.lo = 0; v.hi = UINT32_MAX; break;
    // i64/u64/ptr/float: el tipo no acota nada util.
    default: v.conocido = false; break;
    }
    return v;
}

/// Suma con freno: si se desborda, se deja de afirmar.  Mentir en un extremo
/// es peor que no saberlo, porque el consumidor lo tomaria por demostrado.
bool suma_segura(int64_t a, int64_t b, int64_t &out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return false;
    out = a + b;
    return true;
}

bool mul_segura(int64_t a, int64_t b, int64_t &out) {
    if (a == 0 || b == 0) { out = 0; return true; }
    const int64_t r = a * b;
    if (r / b != a) return false;
    out = r;
    return true;
}

/// Interseccion: quedarse con lo mas estrecho que digan las dos fuentes.
ValueRange estrechar(const ValueRange &a, const ValueRange &b) {
    if (!a.conocido) return b;
    if (!b.conocido) return a;
    ValueRange v;
    v.conocido = true;
    v.lo = std::max(a.lo, b.lo);
    v.hi = std::min(a.hi, b.hi);
    if (v.lo > v.hi) v.conocido = false; // contradiccion -> no se afirma nada
    return v;
}

} // namespace

RangeFacts compute_ranges(const ir::IrFunction &fn, const IrFacts &facts) {
    RangeFacts out;
    out.r.assign(facts.def_of.size(), ValueRange{});

    // Suelo: lo que impone el tipo de cada valor.
    for (ir::IrValueId v = 0; v < fn.values.size() && v < out.r.size(); ++v)
        out.r[v] = del_tipo(fn.values[v].type);

    /* Recorrido en orden de bloque.  Con SSA basta una pasada para lo que se
     * afirma aqui: todo lo que se usa esta definido antes, salvo las PHI de un
     * bucle -- y esas las cubre el hecho de induccion, no la propagacion. */
    for (const ir::IrBlock &b : fn.blocks) {
        for (const ir::IrInstr &in : b.instrs) {
            if (in.dst == ir::IR_NO_VALUE || in.dst >= out.r.size()) continue;
            ValueRange nuevo;
            const auto &ops = in.operands;
            auto rango = [&](size_t i) -> const ValueRange & {
                static const ValueRange kNada{};
                return (i < ops.size() && ops[i] < out.r.size()) ? out.r[ops[i]]
                                                                 : kNada;
            };
            switch (in.op) {
            case IrOp::CONST:
                nuevo.conocido = true;
                nuevo.lo = nuevo.hi = static_cast<int64_t>(in.imm);
                break;
            case IrOp::MOV:
            case IrOp::BITCAST:
                nuevo = rango(0);
                break;
            case IrOp::ADD: {
                const ValueRange &a = rango(0), &c = rango(1);
                if (a.conocido && c.conocido &&
                    suma_segura(a.lo, c.lo, nuevo.lo) &&
                    suma_segura(a.hi, c.hi, nuevo.hi))
                    nuevo.conocido = true;
                break;
            }
            case IrOp::SUB: {
                const ValueRange &a = rango(0), &c = rango(1);
                if (a.conocido && c.conocido &&
                    suma_segura(a.lo, -c.hi, nuevo.lo) &&
                    suma_segura(a.hi, -c.lo, nuevo.hi))
                    nuevo.conocido = true;
                break;
            }
            case IrOp::MUL: {
                const ValueRange &a = rango(0), &c = rango(1);
                // Solo con los dos extremos no negativos: con signos mezclados
                // el minimo y el maximo no son los productos de los extremos.
                if (a.conocido && c.conocido && a.lo >= 0 && c.lo >= 0 &&
                    mul_segura(a.lo, c.lo, nuevo.lo) &&
                    mul_segura(a.hi, c.hi, nuevo.hi))
                    nuevo.conocido = true;
                break;
            }
            case IrOp::AND: {
                /* Una mascara acota por arriba sin saber nada del otro lado:
                 * `x & 0x0F` no pasa de 15, venga x de donde venga. */
                const ValueRange &a = rango(0), &c = rango(1);
                const bool a_masc = a.conocido && a.lo == a.hi && a.lo >= 0;
                const bool c_masc = c.conocido && c.lo == c.hi && c.lo >= 0;
                if (a_masc || c_masc) {
                    nuevo.conocido = true;
                    nuevo.lo = 0;
                    nuevo.hi = a_masc ? (c_masc ? std::min(a.hi, c.hi) : a.hi)
                                      : c.hi;
                }
                break;
            }
            case IrOp::ZEXT:
            case IrOp::TRUNC:
                // El destino ya trae el suelo de su tipo; estrecharlo con el
                // origen solo vale para ZEXT (no cambia el valor).
                if (in.op == IrOp::ZEXT) nuevo = estrechar(out.r[in.dst], rango(0));
                else nuevo = out.r[in.dst];
                break;
            default:
                nuevo = out.r[in.dst]; // se queda con lo que diga su tipo
                break;
            }
            if (nuevo.conocido) out.r[in.dst] = estrechar(out.r[in.dst], nuevo);
        }
    }

    /* Variables de induccion: el rango sale de los hechos de bucle QUE YA
     * EXISTEN (`compute_loop_facts` + `detect_loop_structure` + `detect_loop_iv`),
     * no de re-descubrir la forma del bucle aqui.  Un IV que arranca en un valor
     * conocido y crece de S en S mientras `iv < N` esta entre el inicio y N. */
    {
        const LoopFacts lf = compute_loop_facts(fn);
        std::vector<int> def_block(facts.def_of.size(), -1);
        for (uint32_t bi = 0; bi < fn.blocks.size(); ++bi)
            for (const ir::IrInstr &in : fn.blocks[bi].instrs)
                if (in.dst != ir::IR_NO_VALUE && in.dst < def_block.size())
                    def_block[in.dst] = static_cast<int>(bi);
        std::vector<uint8_t> visto(fn.blocks.size(), 0);
        for (uint32_t bi = 0; bi < fn.blocks.size(); ++bi) {
            if (bi >= lf.is_loop_header.size() || !lf.is_loop_header[bi]) continue;
            const uint32_t lid = lf.loop_id[bi];
            if (lid == LoopFacts::NO_LOOP || visto[bi]) continue;
            visto[bi] = 1;
            const LoopStructure ls = detect_loop_structure(fn, lf, lid);
            if (!ls.valid) continue;
            LoopIV iv;
            if (!detect_loop_iv(fn, def_block, ls.header, ls.preheader, ls.latch, iv))
                continue;
            if (iv.phi == ir::IR_NO_VALUE || iv.phi >= out.r.size()) continue;
            if (iv.init >= out.r.size() || iv.bound >= out.r.size()) continue;
            const ValueRange ini = out.r[iv.init];
            const ValueRange cota = out.r[iv.bound];
            if (!ini.conocido || !cota.conocido || iv.stride <= 0) continue;
            ValueRange v;
            v.conocido = true;
            v.lo = ini.lo;
            /* `iv < N` deja el ultimo valor en N-1; `iv <= N`, en N.  Con el
             * paso el ultimo puede quedarse corto, nunca pasarse. */
            const bool inclusiva = (iv.cmp_op == IrOp::CMP_LE || iv.cmp_op == IrOp::CMP_ULE);
            v.hi = inclusiva ? cota.hi : (cota.hi > INT64_MIN ? cota.hi - 1 : cota.hi);
            v.hi -= iv.cmp_offset; // la guarda compara `iv + c`, no `iv`
            if (v.hi >= v.lo) out.r[iv.phi] = estrechar(out.r[iv.phi], v);
        }
    }

    return out;
}

} // namespace analysis
