/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file value_range.cpp
 * @brief Motor de rangos sensible al flujo (contrato en value_range.h).
 *
 * Organizacion: la aritmetica del DOMINIO va en `dom` -- operaciones sobre
 * intervalos, todas sin desbordar --, la semantica de cada op del IR en
 * `transferir`, y el recorrido del CFG en `Motor`.  Separado a proposito: el
 * dominio se puede probar solo, y el motor no tiene que saber como se
 * multiplican dos intervalos.
 */
#include "analysis/facts/value_range.h"

#include "analysis/facts/loop_facts.h"
#include "ir/ssa_ir.h"

#include <algorithm>
#include <deque>

namespace analysis {

char RangeAnalysis::ID = 0;

namespace {

using ir::IrOp;
using ir::IrType;

// ===========================================================================
//  Aritmetica del dominio.  NADA aqui puede desbordar: en C++ el
//  desbordamiento con signo es comportamiento indefinido, asi que "calcular y
//  mirar despues" no vale -- para cuando se mira, el dano ya esta hecho.
// ===========================================================================
namespace dom {

bool suma(int64_t a, int64_t b, int64_t &out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_add_overflow(a, b, &out);
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
    out = a + b;
    return true;
#endif
}

bool resta(int64_t a, int64_t b, int64_t &out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_sub_overflow(a, b, &out);
#else
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return false;
    out = a - b;
    return true;
#endif
}

bool mul(int64_t a, int64_t b, int64_t &out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_mul_overflow(a, b, &out);
#else
    if (a == 0 || b == 0) { out = 0; return true; }
    if (a > 0) {
        if (b > 0) { if (a > INT64_MAX / b) return false; }
        else       { if (b < INT64_MIN / a) return false; }
    } else {
        if (b > 0) { if (a < INT64_MIN / b) return false; }
        else       { if (a < INT64_MAX / b) return false; }
    }
    out = a * b;
    return true;
#endif
}

bool inc(int64_t a, int64_t &out) { return suma(a, 1, out); }
bool dec(int64_t a, int64_t &out) { return resta(a, 1, out); }

/// Suma de intervalos.  Si un extremo no cabe, no se afirma nada: mejor TOP
/// que un extremo inventado, que el consumidor tomaria por demostrado.
ValueRange sumar(const ValueRange &a, const ValueRange &b) {
    if (a.es_bottom() || b.es_bottom()) return ValueRange::bottom();
    if (!a.acotada() || !b.acotada()) return ValueRange::top();
    int64_t l, h;
    if (!suma(a.lo, b.lo, l) || !suma(a.hi, b.hi, h)) return ValueRange::top();
    return ValueRange::acotado(l, h);
}

ValueRange restar(const ValueRange &a, const ValueRange &b) {
    if (a.es_bottom() || b.es_bottom()) return ValueRange::bottom();
    if (!a.acotada() || !b.acotada()) return ValueRange::top();
    int64_t l, h;
    // [a.lo - b.hi, a.hi - b.lo].  Se resta directamente: negar b.hi seria UB
    // cuando vale INT64_MIN.
    if (!resta(a.lo, b.hi, l) || !resta(a.hi, b.lo, h)) return ValueRange::top();
    return ValueRange::acotado(l, h);
}

/// Producto de intervalos por las CUATRO esquinas.  Con signos mezclados el
/// minimo y el maximo no son los productos de los extremos homologos, asi que
/// mirar solo dos deja fuera casos perfectamente calculables.
ValueRange multiplicar(const ValueRange &a, const ValueRange &b) {
    if (a.es_bottom() || b.es_bottom()) return ValueRange::bottom();
    if (!a.acotada() || !b.acotada()) return ValueRange::top();
    int64_t p[4];
    if (!mul(a.lo, b.lo, p[0]) || !mul(a.lo, b.hi, p[1]) ||
        !mul(a.hi, b.lo, p[2]) || !mul(a.hi, b.hi, p[3]))
        return ValueRange::top();
    return ValueRange::acotado(*std::min_element(p, p + 4),
                               *std::max_element(p, p + 4));
}

ValueRange negar(const ValueRange &a) {
    if (a.es_bottom()) return ValueRange::bottom();
    if (!a.acotada()) return ValueRange::top();
    int64_t l, h;
    if (!resta(0, a.hi, l) || !resta(0, a.lo, h)) return ValueRange::top();
    return ValueRange::acotado(l, h);
}

/// `x & c` con `c` constante no negativa no pasa de `c`, venga x de donde
/// venga.  Es lo unico que se puede afirmar sin mirar los bits del otro lado.
ValueRange conjuncion(const ValueRange &a, const ValueRange &b) {
    if (a.es_bottom() || b.es_bottom()) return ValueRange::bottom();
    auto cte_no_neg = [](const ValueRange &r) {
        return r.acotada() && r.lo == r.hi && r.lo >= 0;
    };
    const bool ca = cte_no_neg(a), cb = cte_no_neg(b);
    if (!ca && !cb) return ValueRange::top();
    if (ca && cb) return ValueRange::acotado(0, std::min(a.hi, b.hi));
    return ValueRange::acotado(0, ca ? a.hi : b.hi);
}

/// Sube el extremo inferior sin desbordar; si no cabe, deja el intervalo igual.
ValueRange con_piso(const ValueRange &r, int64_t piso) {
    if (!r.acotada()) return r;
    return ValueRange::acotado(std::max(r.lo, piso), r.hi);
}
ValueRange con_tope(const ValueRange &r, int64_t tope) {
    if (!r.acotada()) return r;
    return ValueRange::acotado(r.lo, std::min(r.hi, tope));
}

} // namespace dom

// ===========================================================================
//  Semantica de los tipos del IR
// ===========================================================================

/// Bits del tipo (0 = no es un entero con ancho definido).
int bits_de(IrType t) {
    switch (t) {
    case IrType::BOOL: return 1;
    case IrType::I8: case IrType::U8: return 8;
    case IrType::I16: case IrType::U16: return 16;
    case IrType::I32: case IrType::U32: case IrType::HANDLE: return 32;
    case IrType::I64: case IrType::U64: case IrType::PTR: return 64;
    default: return 0;
    }
}

bool es_sin_signo(IrType t) {
    switch (t) {
    case IrType::U8: case IrType::U16: case IrType::U32: case IrType::U64:
    case IrType::BOOL: case IrType::HANDLE: case IrType::PTR:
        return true;
    default:
        return false;
    }
}

/// Rango que impone el ANCHO del tipo.  Un `u8` no pasa de 255 en ninguna
/// arquitectura; es el suelo de conocimiento y sale gratis.  Los de 64 bits no
/// acotan nada representable en `int64_t`, asi que ahi es TOP.
ValueRange del_tipo(IrType t) {
    switch (t) {
    case IrType::I8: return ValueRange::acotado(-128, 127);
    case IrType::I16: return ValueRange::acotado(-32768, 32767);
    case IrType::I32: return ValueRange::acotado(INT32_MIN, INT32_MAX);
    case IrType::U8: return ValueRange::acotado(0, 255);
    case IrType::U16: return ValueRange::acotado(0, 65535);
    case IrType::U32: return ValueRange::acotado(0, UINT32_MAX);
    case IrType::BOOL: return ValueRange::acotado(0, 1);
    /* HANDLE es una referencia opaca, no una cantidad: acotarla por su ancho
     * invitaria a que la aritmetica explotara un "rango" que no significa
     * nada. */
    default: return ValueRange::top();
    }
}

// ===========================================================================
//  Estado por punto del programa
// ===========================================================================

/**
 * @brief Refinamientos sobre el suelo del tipo, en un punto concreto.
 *
 * Disperso a proposito: lo que un camino sabe de mas son unas pocas variables
 * acotadas por una guarda, asi que un vector denso por bloque pagaria
 * bloques x valores para repetir lo que ya dice el tipo.
 *
 * CONVENIO que respetan todos los operadores: la AUSENCIA de una entrada
 * significa "lo que diga el suelo", no "no se".
 *
 * `alcanzable = false` es BOTTOM del estado entero: a este punto no se llega.
 * No es lo mismo que un estado vacio, que significa "se llega y no se sabe nada
 * de mas".
 */
struct Estado {
    bool alcanzable = false;
    std::vector<std::pair<ir::IrValueId, ValueRange>> ref;

    static std::vector<std::pair<ir::IrValueId, ValueRange>>::const_iterator
    pos(const std::vector<std::pair<ir::IrValueId, ValueRange>> &v,
        ir::IrValueId k) {
        return std::lower_bound(
            v.begin(), v.end(), k,
            [](const std::pair<ir::IrValueId, ValueRange> &p, ir::IrValueId x) {
                return p.first < x;
            });
    }
    const ValueRange *buscar(ir::IrValueId v) const {
        auto it = pos(ref, v);
        return (it == ref.end() || it->first != v) ? nullptr : &it->second;
    }
    void poner(ir::IrValueId v, const ValueRange &r) {
        auto it = std::lower_bound(
            ref.begin(), ref.end(), v,
            [](const std::pair<ir::IrValueId, ValueRange> &p, ir::IrValueId x) {
                return p.first < x;
            });
        if (it != ref.end() && it->first == v) it->second = r;
        else ref.insert(it, {v, r});
    }
    bool operator==(const Estado &o) const {
        if (alcanzable != o.alcanzable) return false;
        if (!alcanzable) return true; // dos inalcanzables son el mismo estado
        if (ref.size() != o.ref.size()) return false;
        for (size_t i = 0; i < ref.size(); ++i)
            if (ref[i].first != o.ref[i].first || !(ref[i].second == o.ref[i].second))
                return false;
        return true;
    }
};

/// Arista del CFG con la condicion que la guarda (si la hay).
struct Arista {
    ir::IrBlockId desde = 0, hasta = 0;
    ir::IrValueId cond = ir::IR_NO_VALUE;
    bool rama = true;
};

// ===========================================================================
//  Motor
// ===========================================================================

struct Motor {
    const ir::IrFunction &fn;
    const IrFacts        &facts;
    const RangeOptions   &op;

    std::vector<ValueRange> suelo;
    std::vector<Arista>     aristas;
    std::vector<Estado>     out_arista;
    std::vector<Estado>     in_bloque;
    std::vector<std::vector<uint32_t>> entrantes, salientes;
    std::vector<uint8_t>    es_cabecera;
    std::vector<uint32_t>   revisitas;

    Motor(const ir::IrFunction &f, const IrFacts &fc, const RangeOptions &o)
        : fn(f), facts(fc), op(o) {
        const size_t n = fc.def_of.size();
        suelo.assign(n, ValueRange::top());
        for (ir::IrValueId v = 0; v < fn.values.size() && v < n; ++v) {
            suelo[v] = del_tipo(fn.values[v].type);
            if (fn.values[v].is_const)
                suelo[v] = suelo[v].cortar(ValueRange::constante(
                    static_cast<int64_t>(fn.values[v].const_val)));
        }
        const size_t nb = fn.blocks.size();
        in_bloque.assign(nb, Estado{});
        entrantes.assign(nb, {});
        salientes.assign(nb, {});
        revisitas.assign(nb, 0);
        es_cabecera.assign(nb, 0);
        construir_aristas();
        out_arista.assign(aristas.size(), Estado{});
        const LoopFacts lf = compute_loop_facts(fn);
        for (uint32_t bi = 0; bi < nb && bi < lf.is_loop_header.size(); ++bi)
            es_cabecera[bi] = lf.is_loop_header[bi];
    }

    void construir_aristas() {
        const size_t nb = fn.blocks.size();
        for (uint32_t bi = 0; bi < nb; ++bi) {
            if (fn.blocks[bi].instrs.empty()) continue;
            const ir::IrInstr &t = fn.blocks[bi].instrs.back();
            auto anadir = [&](ir::IrBlockId d, ir::IrValueId c, bool r) {
                if (d == ir::IR_NO_BLOCK || d >= nb) return;
                const uint32_t id = static_cast<uint32_t>(aristas.size());
                aristas.push_back({bi, d, c, r});
                salientes[bi].push_back(id);
                entrantes[d].push_back(id);
            };
            if (t.op == IrOp::BR) {
                anadir(t.target_block, ir::IR_NO_VALUE, true);
            } else if (t.op == IrOp::BR_COND) {
                const ir::IrValueId c =
                    t.operands.empty() ? ir::IR_NO_VALUE : t.operands[0];
                anadir(t.target_block, c, true);
                anadir(t.false_block, c, false);
            } else if (t.op == IrOp::SWITCH_DENSE || t.op == IrOp::MATCH_VARIANT) {
                /* Cada case afirma un valor concreto del selector, pero eso
                 * todavia no se modela: haria falta llevar el valor del case en
                 * la arista.  Mientras tanto no se afirma nada por estas
                 * aristas -- que es correcto, solo menos preciso. */
                for (uint32_t d : t.jump_targets) anadir(d, ir::IR_NO_VALUE, true);
                anadir(t.target_block, ir::IR_NO_VALUE, true);
            }
        }
    }

    ValueRange valor(const Estado &e, ir::IrValueId v) const {
        if (v == ir::IR_NO_VALUE || v >= suelo.size()) return ValueRange::top();
        if (!e.alcanzable) return ValueRange::bottom();
        if (const ValueRange *r = e.buscar(v)) return *r;
        return suelo[v];
    }

    /// Union de estados respetando el convenio (la ausencia es el suelo) y
    /// BOTTOM (un camino inalcanzable no aporta valores, no los destruye).
    Estado unir_estados(const Estado &a, const Estado &b) const {
        if (!a.alcanzable) return b;
        if (!b.alcanzable) return a;
        Estado out;
        out.alcanzable = true;
        for (const auto &p : a.ref) {
            const ValueRange *q = b.buscar(p.first);
            const ValueRange u = p.second.unir(q ? *q : suelo[p.first]);
            if (!u.es_top()) out.ref.push_back({p.first, u});
        }
        return out;
    }

    Estado ensanchar_estado(const Estado &viejo, const Estado &nuevo) const {
        if (!viejo.alcanzable || !nuevo.alcanzable) return nuevo;
        Estado out;
        out.alcanzable = true;
        for (const auto &p : nuevo.ref) {
            const ValueRange *v = viejo.buscar(p.first);
            const ValueRange base = v ? *v : suelo[p.first];
            const ValueRange w = base.ensanchar(p.second, suelo[p.first]);
            if (!w.es_top()) out.ref.push_back({p.first, w});
        }
        return out;
    }

    /**
     * @brief Lo que una guarda AFIRMA sobre una arista, en las DOS ramas.
     *
     * Saber que algo no se cumple informa tanto como saber que si.  Y si lo
     * afirmado contradice lo que ya se sabia, el resultado NO es "no se": es
     * que por esa arista no se pasa -- `x = 20; if (x < 10)` --, y eso se
     * propaga como BOTTOM.
     */
    void estrechar_por_guarda(Estado &e, ir::IrValueId cond, bool rama) const {
        if (!e.alcanzable) return;
        const ir::IrInstr *d = facts.def(cond);
        if (!d || d->operands.size() != 2) return;
        const ir::IrValueId va = d->operands[0], vb = d->operands[1];
        IrOp o = d->op;
        if (!rama) {
            switch (o) {
            case IrOp::CMP_LT: o = IrOp::CMP_GE; break;
            case IrOp::CMP_LE: o = IrOp::CMP_GT; break;
            case IrOp::CMP_GT: o = IrOp::CMP_LE; break;
            case IrOp::CMP_GE: o = IrOp::CMP_LT; break;
            case IrOp::CMP_ULT: o = IrOp::CMP_UGE; break;
            case IrOp::CMP_ULE: o = IrOp::CMP_UGT; break;
            case IrOp::CMP_UGT: o = IrOp::CMP_ULE; break;
            case IrOp::CMP_UGE: o = IrOp::CMP_ULT; break;
            case IrOp::CMP_EQ: o = IrOp::CMP_NE; break;
            case IrOp::CMP_NE: o = IrOp::CMP_EQ; break;
            default: return;
            }
        }
        const ValueRange ra = valor(e, va), rb = valor(e, vb);
        const bool sin_signo = (o == IrOp::CMP_ULT || o == IrOp::CMP_ULE ||
                                o == IrOp::CMP_UGT || o == IrOp::CMP_UGE);
        /* Un valor sin signo de 64 bits por encima de INT64_MAX cae negativo en
         * esta representacion; ahi el estrechamiento afirmaria al reves. */
        if (sin_signo && ((ra.acotada() && ra.lo < 0) || (rb.acotada() && rb.lo < 0)))
            return;

        auto aplicar = [&](ir::IrValueId v, const ValueRange &r) {
            if (v == ir::IR_NO_VALUE || v >= suelo.size()) return;
            if (r.es_bottom()) { e.alcanzable = false; e.ref.clear(); return; }
            if (!r.es_top()) e.poner(v, r);
        };
        /* Una comparacion SIN SIGNO acota por si sola: `ult(i, 200)` demuestra
         * `i en [0,199]` venga i de donde venga -- lo dice la comparacion, no lo
         * que se supiera antes.  Con signo el extremo inferior queda abierto y
         * un intervalo cerrado no lo representa. */
        const ValueRange base_a = (sin_signo && !ra.acotada())
                                      ? ValueRange::acotado(0, INT64_MAX)
                                      : ra;
        const ValueRange base_b = (sin_signo && !rb.acotada())
                                      ? ValueRange::acotado(0, INT64_MAX)
                                      : rb;
        int64_t t;
        switch (o) {
        case IrOp::CMP_LT: case IrOp::CMP_ULT:
            if (rb.acotada() && dom::dec(rb.hi, t)) aplicar(va, dom::con_tope(base_a, t));
            if (ra.acotada() && dom::inc(ra.lo, t)) aplicar(vb, dom::con_piso(base_b, t));
            break;
        case IrOp::CMP_LE: case IrOp::CMP_ULE:
            if (rb.acotada()) aplicar(va, dom::con_tope(base_a, rb.hi));
            if (ra.acotada()) aplicar(vb, dom::con_piso(base_b, ra.lo));
            break;
        case IrOp::CMP_GT: case IrOp::CMP_UGT:
            if (rb.acotada() && dom::inc(rb.lo, t)) aplicar(va, dom::con_piso(base_a, t));
            if (ra.acotada() && dom::dec(ra.hi, t)) aplicar(vb, dom::con_tope(base_b, t));
            break;
        case IrOp::CMP_GE: case IrOp::CMP_UGE:
            if (rb.acotada()) aplicar(va, dom::con_piso(base_a, rb.lo));
            if (ra.acotada()) aplicar(vb, dom::con_tope(base_b, ra.hi));
            break;
        case IrOp::CMP_EQ:
            aplicar(va, ra.cortar(rb));
            if (e.alcanzable) aplicar(vb, rb.cortar(ra));
            break;
        default:
            break; // CMP_NE sobre intervalos no estrecha salvo en los extremos
        }
    }

    /// Reinterpretacion entre anchos.  Un `zext` NO conserva el valor si el
    /// origen era negativo: `i8 -1` extendido a `u32` vale 255, no -1.
    ValueRange convertir(IrOp o, const ir::IrInstr &in, const ValueRange &src) const {
        const IrType t_dst = in.type;
        const IrType t_src = (!in.operands.empty() && in.operands[0] < fn.values.size())
                                 ? fn.values[in.operands[0]].type
                                 : IrType::VOID;
        const int w = bits_de(t_src);
        const ValueRange piso_dst = del_tipo(t_dst);
        if (src.es_bottom()) return ValueRange::bottom();
        switch (o) {
        case IrOp::SEXT:
            // Preserva el VALOR; el destino es mas ancho, asi que siempre cabe.
            return acotar_al_tipo(src, piso_dst);
        case IrOp::ZEXT:
            if (src.acotada() && src.lo >= 0) return acotar_al_tipo(src, piso_dst);
            if (w > 0 && w < 63) // [0, 2^w - 1] cabe en int64
                return acotar_al_tipo(ValueRange::acotado(0, (int64_t(1) << w) - 1), piso_dst);
            return piso_dst;
        case IrOp::TRUNC: {
            // Solo se conserva si el valor entero cabe ya en el destino; si no,
            // lo unico afirmable es el ancho del destino.
            const ValueRange corte = acotar_al_tipo(src, piso_dst);
            if (src.acotada() && piso_dst.acotada() && src.lo >= piso_dst.lo &&
                src.hi <= piso_dst.hi)
                return corte;
            return piso_dst;
        }
        default:
            return piso_dst;
        }
    }

    /**
     * @brief Encaja un resultado en su tipo sin afirmar de mas ni de menos.
     *
     * Si cabe entero, se queda como esta.  Si NO cabe, la operacion envolvio y
     * el valor real es otro: lo unico afirmable es el rango del tipo.  Nunca
     * BOTTOM -- que un resultado se salga del tipo no significa que el programa
     * no pase por ahi.
     */
    static ValueRange acotar_al_tipo(const ValueRange &r, const ValueRange &tipo) {
        if (r.es_bottom()) return r;          // venia de un camino imposible
        if (!r.acotada()) return tipo;        // no se sabe: lo que diga el tipo
        if (!tipo.acotada()) return r;        // el tipo no acota (64 bits)
        if (r.lo >= tipo.lo && r.hi <= tipo.hi) return r;
        return tipo;
    }

    /// Transferencia de una instruccion (las PHI se resuelven en IN[B]).
    void transferir(const ir::IrInstr &in, Estado &e) const {
        if (!e.alcanzable) return;
        if (in.dst == ir::IR_NO_VALUE || in.dst >= suelo.size()) return;
        auto arg = [&](size_t i) {
            return i < in.operands.size() ? valor(e, in.operands[i])
                                          : ValueRange::top();
        };
        ValueRange nuevo;
        switch (in.op) {
        case IrOp::CONST:
            nuevo = ValueRange::constante(static_cast<int64_t>(in.imm));
            break;
        case IrOp::MOV:
            nuevo = arg(0);
            break;
        case IrOp::ADD: nuevo = dom::sumar(arg(0), arg(1)); break;
        case IrOp::SUB: nuevo = dom::restar(arg(0), arg(1)); break;
        case IrOp::MUL: nuevo = dom::multiplicar(arg(0), arg(1)); break;
        case IrOp::NEG: nuevo = dom::negar(arg(0)); break;
        case IrOp::AND: nuevo = dom::conjuncion(arg(0), arg(1)); break;
        case IrOp::SEXT:
        case IrOp::ZEXT:
        case IrOp::TRUNC:
            nuevo = convertir(in.op, in, arg(0));
            break;
        /* BITCAST reinterpreta BITS, no valores.  Entre un float y un entero el
         * rango numerico del origen no dice absolutamente nada del destino, asi
         * que se pierde.
         *
         * Pero entre DOS ENTEROS DEL MISMO ANCHO y con el origen no negativo, el
         * valor SI se conserva: un numero no negativo tiene la misma
         * representacion leido con signo o sin el.  Y ese caso no es exotico --
         * es como el IR pasa un indice `u64` a una suma de punteros --, asi que
         * tirarlo dejaba fuera de comprobacion justo los accesos indexados. */
        case IrOp::BITCAST: {
            const ValueRange s = arg(0);
            const IrType t_s =
                (!in.operands.empty() && in.operands[0] < fn.values.size())
                    ? fn.values[in.operands[0]].type
                    : IrType::VOID;
            const int b_s = bits_de(t_s), b_d = bits_de(in.type);
            if (b_s > 0 && b_s == b_d && s.acotada() && s.lo >= 0)
                nuevo = s;
            else
                nuevo = ValueRange::top();
            break;
        }
        default:
            nuevo = ValueRange::top();
            break;
        }
        /* El resultado se ACOTA por el tipo, no se corta contra el.
         *
         * La diferencia es de correccion, no de precision: la aritmetica del IR
         * ENVUELVE al ancho del tipo, asi que un `u8` con 250 + 10 vale 4, no
         * 260.  Cortar [260,260] contra el suelo [0,255] da un intervalo vacio,
         * o sea BOTTOM, o sea "aqui no se llega" -- afirmando que un punto
         * perfectamente alcanzable no lo es.  Ese es el peor error que puede
         * cometer un analisis: no pierde precision, miente.
         *
         * Cuando el resultado no cabe en el tipo, lo unico que se puede afirmar
         * es el propio tipo. */
        e.poner(in.dst, acotar_al_tipo(nuevo, suelo[in.dst]));
    }

    /// IN[B]: union de lo que llega por cada arista, con las PHI resueltas
    /// DESDE EL ESTADO DE SU ARISTA.
    Estado calcular_in(ir::IrBlockId bi) const {
        Estado e;
        for (uint32_t ai : entrantes[bi])
            if (out_arista[ai].alcanzable) e = unir_estados(e, out_arista[ai]);
        if (bi == 0) e.alcanzable = true; // la entrada siempre se alcanza
        if (!e.alcanzable) return e;
        resolver_phis(bi, e);
        return e;
    }

    /// Cada PHI vale la union de lo que trae cada arista entrante, leido EN esa
    /// arista.  Es lo que hace el analisis sensible al flujo y no una
    /// propagacion global por valor.
    void resolver_phis(ir::IrBlockId bi, Estado &e) const {
        for (const ir::IrInstr &in : fn.blocks[bi].instrs) {
            if (in.op != IrOp::PHI) break; // van al principio del bloque
            if (in.dst == ir::IR_NO_VALUE || in.dst >= suelo.size()) continue;
            ValueRange acc = ValueRange::bottom();
            for (const ir::IrPhiArg &pa : in.phi_args)
                for (uint32_t ai : entrantes[bi])
                    if (aristas[ai].desde == pa.block && out_arista[ai].alcanzable)
                        acc = acc.unir(valor(out_arista[ai], pa.value));
            /* Mismo criterio que en la transferencia: acotar por el tipo, no
             * cortar contra el.  Cortar convertiria un valor que no encaja en
             * "este punto no se alcanza", que es mentir. */
            e.poner(in.dst, acotar_al_tipo(acc, suelo[in.dst]));
        }
    }

    Estado calcular_out(ir::IrBlockId bi, const Estado &in) const {
        Estado e = in;
        for (const ir::IrInstr &instr : fn.blocks[bi].instrs)
            if (instr.op != IrOp::PHI) transferir(instr, e);
        return e;
    }

    /**
     * @brief Punto fijo por lista de trabajo.
     *
     * @param ensanchar  ASCENSO: se ensancha en las cabeceras (termina).
     *                   DESCENSO: se reemplaza el estado (recupera precision).
     * @return true si la lista se vacio sola.
     */
    bool resolver(bool ensanchar, int presupuesto) {
        if (fn.blocks.empty()) return true;
        std::deque<ir::IrBlockId> cola;
        cola.push_back(0);
        int pasos = 0;
        while (!cola.empty()) {
            if (++pasos > presupuesto) return false;
            const ir::IrBlockId bi = cola.front();
            cola.pop_front();

            Estado nuevo_in = calcular_in(bi);
            if (ensanchar && es_cabecera[bi] && revisitas[bi] >= op.retardo_ensanche)
                nuevo_in = ensanchar_estado(in_bloque[bi], nuevo_in);
            if (!(nuevo_in == in_bloque[bi])) {
                in_bloque[bi] = std::move(nuevo_in);
                revisitas[bi]++;
            }
            if (!in_bloque[bi].alcanzable) continue;

            const Estado out = calcular_out(bi, in_bloque[bi]);
            for (uint32_t ai : salientes[bi]) {
                Estado se = out;
                if (aristas[ai].cond != ir::IR_NO_VALUE)
                    estrechar_por_guarda(se, aristas[ai].cond, aristas[ai].rama);
                if (!(se == out_arista[ai])) {
                    out_arista[ai] = std::move(se);
                    cola.push_back(aristas[ai].hasta);
                }
            }
        }
        return true;
    }

    /// Proyeccion final: el rango de cada valor en SU PUNTO DE DEFINICION.
    std::vector<ValueRange> en_definicion() const {
        std::vector<ValueRange> out = suelo;
        for (uint32_t bi = 0; bi < fn.blocks.size(); ++bi) {
            if (!in_bloque[bi].alcanzable) continue;
            Estado e = in_bloque[bi];
            for (const ir::IrInstr &in : fn.blocks[bi].instrs) {
                if (in.op != IrOp::PHI) transferir(in, e);
                if (in.dst != ir::IR_NO_VALUE && in.dst < out.size())
                    out[in.dst] = valor(e, in.dst);
            }
        }
        return out;
    }
};

} // namespace

RangeFacts compute_ranges(const ir::IrFunction &fn, const IrFacts &facts,
                          const RangeOptions &op) {
    RangeFacts out;
    Motor m(fn, facts, op);
    if (fn.blocks.empty()) {
        out.r = m.suelo;
        return out;
    }
    const int presupuesto =
        static_cast<int>(op.pasos_por_bloque * fn.blocks.size() + op.pasos_extra);

    // Ascenso con ensanchamiento en cabeceras: es lo que garantiza terminar.
    const bool ok = m.resolver(/*ensanchar=*/true, presupuesto);
    // Descenso sin ensanchar, partiendo de una solucion ya estable: devuelve la
    // precision que el ensanchamiento solto.
    bool ok2 = true;
    if (ok) {
        std::fill(m.revisitas.begin(), m.revisitas.end(), 0u);
        ok2 = m.resolver(/*ensanchar=*/false, presupuesto);
    }

    out.vueltas = 0;
    for (uint32_t v : m.revisitas) out.vueltas = std::max(out.vueltas, (int)v);
    out.convergio = ok && ok2;
    if (!out.convergio) {
        // Sin punto fijo no hay hecho que sostener: no se afirma nada.
        out.r.assign(facts.def_of.size(), ValueRange::top());
        return out;
    }
    out.r = m.en_definicion();
    return out;
}

} // namespace analysis
