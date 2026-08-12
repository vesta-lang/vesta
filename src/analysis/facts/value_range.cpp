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
 * Aqui NO hay semantica de tipos.  Ni envoltura, ni signo, ni extensiones: todo
 * eso vive en el dominio (@c value_range_domain.cpp) y se prueba sin construir
 * una funcion.  Este fichero solo hace tres cosas:
 *
 *   traducir    que operacion del dominio corresponde a cada op del IR
 *   recorrer    el CFG llevando el estado por bloque y por ARISTA
 *   terminar    ensanchando en las aristas de retroceso, y luego estrechando
 *
 * Esa division es la que permite creerse el resultado: si el dominio no miente,
 * lo unico que puede fallar aqui es el recorrido, y el recorrido es pequeno.
 */
#include "analysis/facts/value_range.h"

#include "analysis/facts/loop_facts.h"
#include "analysis/facts/range_summary.h"
#include "analysis/memory/fn_targets.h"
#include "ir/ssa_ir.h"

#include "util/fnv.h"
#include "util/reloj.h"

#include <atomic>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace analysis {

char RangeAnalysis::ID = 0;

namespace {

using ir::IrOp;
using ir::IrType;

// ===========================================================================
//  Tipos del IR -> tipos del dominio
// ===========================================================================

RangeType tipo_de(IrType t) {
    switch (t) {
    case IrType::BOOL: return RangeType::u(1);
    case IrType::I8: return RangeType::i(8);
    case IrType::I16: return RangeType::i(16);
    case IrType::I32: return RangeType::i(32);
    case IrType::I64: return RangeType::i(64);
    case IrType::U8: return RangeType::u(8);
    case IrType::U16: return RangeType::u(16);
    case IrType::U32: return RangeType::u(32);
    case IrType::U64: return RangeType::u(64);
    case IrType::PTR: return RangeType::u(64);
    default: return RangeType::i(64);
    }
}

/// Si el tipo permite razonar numericamente sobre el valor.
bool es_numerico(IrType t) {
    switch (t) {
    case IrType::I8: case IrType::I16: case IrType::I32: case IrType::I64:
    case IrType::U8: case IrType::U16: case IrType::U32: case IrType::U64:
    case IrType::BOOL: case IrType::PTR:
        return true;
    /* HANDLE es una referencia opaca, no una cantidad: acotarla por su ancho
     * invitaria a que la aritmetica explotara un "rango" sin significado. */
    default:
        return false;
    }
}

/// Suelo de conocimiento: lo que impone el tipo, gratis y en cualquier
/// arquitectura.  Un `u8` no pasa de 255; un `u64` no acota nada util pero SI
/// dice que no es negativo, y eso ya sirve.
ValueRange del_tipo(IrType t) {
    if (!es_numerico(t)) return ValueRange::top();
    return ValueRange::todo(tipo_de(t));
}

} // namespace

ValueRange rango_del_tipo(ir::IrType t) { return del_tipo(t); }

namespace {

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
 * DOS INALCANZABILIDADES, y no son la misma:
 *
 *   `alcanzable = false`   propiedad del CFG: a este PUNTO no se llega.
 *   `ValueRange::Bottom`   propiedad de un VALOR: no hay numero que cumpla lo
 *                          que se ha afirmado sobre el en este punto.
 *
 * Lo segundo IMPLICA lo primero (si un valor vivo no puede valer nada, el punto
 * no se ejecuta) y por eso las guardas propagan lo uno a lo otro; pero al reves
 * no: un punto inalcanzable no dice nada de un valor concreto.
 */
/**
 * @brief Contadores del coste de la estructura del estado, por hilo.
 *
 * Van por hilo porque las funciones se analizan en paralelo, y se ponen a cero
 * al empezar cada funcion: lo que se quiere saber es el coste de ESTA, no un
 * acumulado del proceso.  Acaban en @c RangeStats, que es donde vive.
 */
struct CosteEstado {
    uint64_t inserciones = 0;
    uint64_t reescrituras = 0;
    uint64_t copias = 0;
    /* Las dos que faltaban, y que la cuenta de memoria no explicaba: consultar
     * el estado (una busqueda binaria por operando de cada instruccion y por
     * vuelta) y fusionarlo en las confluencias (un vector NUEVO por arista y
     * vuelta, con sus reservas).  Sin contarlas, 300 MB movidos tenian que
     * explicar 8 s, y no daba. */
    uint64_t busquedas = 0;
    uint64_t uniones = 0;
    uint64_t unidos = 0; ///< elementos anadidos al fusionar (reservas del vector)
};
thread_local CosteEstado g_coste;

struct Estado {
    bool alcanzable = false;
    std::vector<std::pair<ir::IrValueId, ValueRange>> ref;

    Estado() = default;
    /// Copiar un estado es el otro coste de la representacion dispersa: hay que
    /// contarlo aqui porque es donde ocurre (una copia por bloque y vuelta).
    Estado(const Estado &o) : alcanzable(o.alcanzable), ref(o.ref) { ++g_coste.copias; }
    Estado &operator=(const Estado &o) {
        if (this != &o) { alcanzable = o.alcanzable; ref = o.ref; ++g_coste.copias; }
        return *this;
    }
    Estado(Estado &&) = default;
    Estado &operator=(Estado &&) = default;

    const ValueRange *buscar(ir::IrValueId v) const {
        ++g_coste.busquedas;
        auto it = std::lower_bound(
            ref.begin(), ref.end(), v,
            [](const std::pair<ir::IrValueId, ValueRange> &p, ir::IrValueId x) {
                return p.first < x;
            });
        return (it == ref.end() || it->first != v) ? nullptr : &it->second;
    }
    void poner(ir::IrValueId v, const ValueRange &r) {
        auto it = std::lower_bound(
            ref.begin(), ref.end(), v,
            [](const std::pair<ir::IrValueId, ValueRange> &p, ir::IrValueId x) {
                return p.first < x;
            });
        if (it != ref.end() && it->first == v) {
            it->second = r;
            ++g_coste.reescrituras;
        } else {
            ref.insert(it, {v, r}); // desplaza todo lo que va detras
            ++g_coste.inserciones;
        }
    }
    void inalcanzable() {
        alcanzable = false;
        ref.clear();
    }
    bool operator==(const Estado &o) const {
        if (alcanzable != o.alcanzable) return false;
        if (!alcanzable) return true; // dos inalcanzables son el mismo estado
        if (ref.size() != o.ref.size()) return false;
        for (size_t i = 0; i < ref.size(); ++i)
            if (ref[i].first != o.ref[i].first || ref[i].second != o.ref[i].second)
                return false;
        return true;
    }
};

/**
 * @brief Arista del CFG, con lo que se puede AFIRMAR al pasar por ella.
 *
 * Dos formas de afirmar, porque hay dos formas de bifurcar:
 *
 *   COMPARACION  `if (x < 10)`: la condicion y por que rama se va.
 *   CASO         el brazo de un `switch`: el selector vale exactamente esto
 *                (o, en el brazo por defecto, cualquier cosa MENOS esto).
 *
 * Sin la segunda, todo el dispatch de un `match` denso entra en sus brazos sin
 * saber el valor del tag, que es justo lo unico que ahi se sabe seguro.
 */
struct Arista {
    ir::IrBlockId desde = 0, hasta = 0;
    // Afirmacion por comparacion.
    ir::IrValueId cond = ir::IR_NO_VALUE;
    bool rama = true;
    // Afirmacion por caso de un switch.
    ir::IrValueId sel = ir::IR_NO_VALUE;
    bool     dentro = true; ///< true: sel esta en [caso_lo,caso_hi]; false: fuera
    uint64_t caso_lo = 0, caso_hi = 0;
    bool retroceso = false; ///< cierra un bucle: es la que obliga a ensanchar
};

// ===========================================================================
//  Contexto: el suelo de los tipos y la transferencia de una instruccion
//
//  Lo comparten el MOTOR (que resuelve el punto fijo) y la CONSULTA (que
//  reproduce un bloque para responder por un punto).  Compartirlo no es un
//  ahorro de lineas: es lo que garantiza que preguntar por un punto y calcular
//  el punto fijo signifiquen exactamente lo mismo.
// ===========================================================================

/**
 * @brief Unica via para leer un resumen, y apunta cada lectura.
 *
 * No es un envoltorio por gusto: es lo que hace que la clave de reuso sea
 * COMPLETA por construccion.  Mientras el analisis no pueda alcanzar el
 * @c RangeSummaries de otra forma, cualquier lectura que se anada manana queda
 * registrada sin que nadie tenga que acordarse de anadirla a ninguna lista.
 *
 * Registra el NOMBRE ademas de la huella para que despues se pueda RELEER lo
 * mismo y comparar; con un total acumulado se sabria que algo cambio, pero no
 * se podria comprobar contra el estado de ahora.
 */
class LectorResumenes {
public:
    explicit LectorResumenes(const RangeSummaries *s) : sum_(s) {}

    const FnRangeSummary *buscar(const std::string &nombre) {
        const FnRangeSummary *s = sum_ ? sum_->buscar(nombre) : nullptr;
        // Se apunta TAMBIEN cuando no hay resumen: "no habia" es un estado, y
        // si manana lo hay el resultado puede cambiar.
        leidas_.emplace_back(nombre, huella_de_resumen(s));
        return s;
    }
    /// @c true si hay resumenes en absoluto (para los caminos que no preguntan).
    bool hay() const { return sum_ != nullptr; }
    std::vector<std::pair<std::string, uint64_t>> soltar() { return std::move(leidas_); }

private:
    const RangeSummaries *sum_ = nullptr;
    std::vector<std::pair<std::string, uint64_t>> leidas_;
};

struct Contexto {
    const ir::IrFunction &fn;
    const IrFacts        &facts;
    mutable LectorResumenes sum{nullptr};
    std::vector<ValueRange> suelo;

    Contexto(const ir::IrFunction &f, const IrFacts &fc,
             const RangeSummaries *s = nullptr)
        : fn(f), facts(fc), sum(s) {
        const size_t n = fc.def_of.size();
        suelo.assign(n, ValueRange::top());
        for (ir::IrValueId v = 0; v < fn.values.size() && v < n; ++v) {
            suelo[v] = del_tipo(fn.values[v].type);
            if (fn.values[v].is_const && suelo[v].acotada())
                suelo[v] = suelo[v].cortar(
                    ValueRange::constante(suelo[v].t, fn.values[v].const_val));
        }
        /* Un parametro vale lo que su tipo... salvo que se sepa quien llama.  El
         * resumen solo estrecha cuando se conocen TODOS los llamantes; si no,
         * trae el mismo suelo y esto no cambia nada. */
        if (sum.hay()) {
            const FnRangeSummary *mio = sum.buscar(fn.name);
            if (mio != nullptr)
                for (size_t i = 0; i < fn.params.size() && i < mio->params.size();
                     ++i) {
                    const ir::IrValueId p = fn.params[i];
                    if (p < suelo.size()) suelo[p] = suelo[p].cortar(mio->params[i]);
                }
        }
    }

    ValueRange valor(const Estado &e, ir::IrValueId v) const {
        if (v == ir::IR_NO_VALUE || v >= suelo.size()) return ValueRange::top();
        if (!e.alcanzable) return ValueRange::bottom(suelo[v].t);
        if (const ValueRange *r = e.buscar(v)) return *r;
        return suelo[v];
    }

    /// Si cabe entero, se queda; si no, lo afirmable es el tipo.  Nunca BOTTOM
    /// por no caber (BOTTOM solo viene de un camino imposible).
    static ValueRange encajar_en(const ValueRange &r, const ValueRange &tipo) {
        if (r.es_bottom()) return r;
        if (!r.acotada()) return tipo;
        if (!tipo.acotada()) return r;
        if (r.t != tipo.t) return tipo;
        const ValueRange c = r.cortar(tipo);
        return c.es_bottom() ? tipo : c;
    }

    /// Que operacion del dominio corresponde a cada op del IR.  Nada mas.
    void transferir(const ir::IrInstr &in, Estado &e) const;
};

// ===========================================================================
//  Motor
// ===========================================================================

struct Motor : Contexto {
    const RangeOptions   &op;

    std::vector<Arista>     aristas;
    std::vector<Estado>     out_arista;
    std::vector<Estado>     in_bloque;
    std::vector<std::vector<uint32_t>> entrantes, salientes;
    std::vector<uint32_t>   vueltas_ciclo; ///< veces que el IN de un bloque cambio
    RangeStats              stats;

    Motor(const ir::IrFunction &f, const IrFacts &fc, const RangeOptions &o,
          const RangeSummaries *s)
        : Contexto(f, fc, s), op(o) {
        const size_t nb = fn.blocks.size();
        in_bloque.assign(nb, Estado{});
        entrantes.assign(nb, {});
        salientes.assign(nb, {});
        vueltas_ciclo.assign(nb, 0);
        construir_aristas();
        out_arista.assign(aristas.size(), Estado{});
    }

    // --- construccion del grafo ------------------------------------------
    void construir_aristas() {
        const size_t nb = fn.blocks.size();
        for (uint32_t bi = 0; bi < nb; ++bi) {
            if (fn.blocks[bi].instrs.empty()) continue;
            const ir::IrInstr &t = fn.blocks[bi].instrs.back();
            auto anadir_arista = [&](Arista a) {
                if (a.hasta == ir::IR_NO_BLOCK || a.hasta >= nb) return;
                const uint32_t id = static_cast<uint32_t>(aristas.size());
                aristas.push_back(a);
                salientes[bi].push_back(id);
                entrantes[a.hasta].push_back(id);
            };
            auto anadir = [&](ir::IrBlockId d, ir::IrValueId c, bool r) {
                Arista a;
                a.desde = bi;
                a.hasta = d;
                a.cond = c;
                a.rama = r;
                anadir_arista(a);
            };
            auto anadir_caso = [&](ir::IrBlockId d, ir::IrValueId sel, bool dentro,
                                   uint64_t lo, uint64_t hi) {
                Arista a;
                a.desde = bi;
                a.hasta = d;
                a.sel = sel;
                a.dentro = dentro;
                a.caso_lo = lo;
                a.caso_hi = hi;
                anadir_arista(a);
            };
            if (t.op == IrOp::BR) {
                anadir(t.target_block, ir::IR_NO_VALUE, true);
            } else if (t.op == IrOp::BR_COND) {
                const ir::IrValueId c =
                    t.operands.empty() ? ir::IR_NO_VALUE : t.operands[0];
                anadir(t.target_block, c, true);
                anadir(t.false_block, c, false);
            } else if (t.op == IrOp::SWITCH_DENSE) {
                /* Tabla densa: el brazo `idx` se toma cuando el selector vale
                 * exactamente `min + idx`, y el brazo por defecto cuando cae
                 * FUERA de toda la tabla.  El minimo viaja en los 32 bits bajos
                 * del inmediato (el bit 32 dice otra cosa: que la comprobacion
                 * de rango sobra porque la tabla cubre el enum entero). */
                const ir::IrValueId sel =
                    t.operands.empty() ? ir::IR_NO_VALUE : t.operands[0];
                const uint64_t min = t.imm & 0xFFFFFFFFu;
                const size_t n = t.jump_targets.size();
                for (size_t idx = 0; idx < n; ++idx)
                    anadir_caso(t.jump_targets[idx], sel, true, min + idx,
                                min + idx);
                if (n > 0)
                    anadir_caso(t.target_block, sel, false, min,
                                min + n - 1);
                else
                    anadir(t.target_block, ir::IR_NO_VALUE, true);
            } else if (t.op == IrOp::MATCH_VARIANT) {
                /* Marcador: el dispatch de verdad es la cadena de comparaciones
                 * que viene detras, y esa ya la lee la guarda.  Aqui solo hay
                 * que no perder los sucesores si acaba cerrando el bloque. */
                for (uint32_t d : t.jump_targets) anadir(d, ir::IR_NO_VALUE, true);
                anadir(t.target_block, ir::IR_NO_VALUE, true);
            }
        }
        marcar_retrocesos();
    }

    /**
     * @brief Marca las aristas que CIERRAN un ciclo.
     *
     * El ensanchamiento no se dispara por "he visitado este bloque muchas
     * veces" -- eso es una consecuencia, no la causa -- sino por volver a una
     * cabecera POR LA ARISTA QUE CIERRA EL BUCLE, que es lo unico que puede
     * hacer crecer un intervalo sin fin.
     */
    void marcar_retrocesos() {
        const LoopFacts lf = compute_loop_facts(fn);
        auto dentro_de = [&](ir::IrBlockId b, uint32_t lid) {
            if (lid == LoopFacts::NO_LOOP) return false;
            uint32_t l = b < lf.loop_id.size() ? lf.loop_id[b] : LoopFacts::NO_LOOP;
            while (l != LoopFacts::NO_LOOP) { // sube por los bucles que lo contienen
                if (l == lid) return true;
                l = l < lf.parent_loop.size() ? lf.parent_loop[l] : LoopFacts::NO_LOOP;
            }
            return false;
        };
        for (Arista &a : aristas) {
            if (a.hasta >= lf.is_loop_header.size() || !lf.is_loop_header[a.hasta])
                continue;
            const uint32_t lid =
                a.hasta < lf.loop_id.size() ? lf.loop_id[a.hasta] : LoopFacts::NO_LOOP;
            a.retroceso = dentro_de(a.desde, lid);
        }
    }

    // --- confluencia --------------------------------------------------------
    Estado unir_estados(const Estado &a, const Estado &b) const {
        if (!a.alcanzable) return b;
        if (!b.alcanzable) return a;
        Estado out;
        ++g_coste.uniones;
        out.alcanzable = true;
        for (const auto &p : a.ref) {
            const ValueRange *q = b.buscar(p.first);
            const ValueRange u = p.second.unir(q ? *q : suelo[p.first]);
            if (!u.es_top()) { out.ref.push_back({p.first, u}); ++g_coste.unidos; }
        }
        return out;
    }

    /// Ensanchamiento del ascenso: por valor, soltando solo el extremo que crece.
    Estado ensanchar_estado(const Estado &viejo, const Estado &nuevo) const {
        if (!viejo.alcanzable || !nuevo.alcanzable) return nuevo;
        Estado out;
        ++g_coste.uniones;
        out.alcanzable = true;
        for (const auto &p : nuevo.ref) {
            const ValueRange *v = viejo.buscar(p.first);
            const ValueRange base = v ? *v : suelo[p.first];
            const ValueRange w = base.ensanchar(p.second);
            if (!w.es_top()) { out.ref.push_back({p.first, w}); ++g_coste.unidos; }
        }
        return out;
    }

    /**
     * @brief Estrechamiento del descenso: se queda con lo mejor de los dos.
     *
     * Nunca declara inalcanzable un valor: si el corte quedara vacio seria un
     * fallo del propio motor (el descenso parte de una solucion estable y solo
     * puede mejorar), y ante eso se conserva lo recien calculado en vez de
     * afirmar que ahi no se llega.
     */
    Estado estrechar_estado(const Estado &viejo, const Estado &nuevo) const {
        if (!nuevo.alcanzable || !viejo.alcanzable) return nuevo;
        Estado out;
        ++g_coste.uniones;
        out.alcanzable = true;
        for (const auto &p : nuevo.ref) {
            const ValueRange *v = viejo.buscar(p.first);
            ValueRange r = p.second;
            if (v) {
                const ValueRange c = r.cortar(*v);
                if (!c.es_bottom()) r = c;
            }
            if (!r.es_top()) { out.ref.push_back({p.first, r}); ++g_coste.unidos; }
        }
        return out;
    }

    // --- guardas ------------------------------------------------------------
    /**
     * @brief Lo que una guarda AFIRMA sobre una arista, en las DOS ramas.
     *
     * Saber que algo no se cumple informa tanto como saber que si.  Y si lo
     * afirmado contradice lo que ya se sabia, el resultado NO es "no se": es
     * que por esa arista no se pasa, y eso se propaga como estado inalcanzable.
     *
     * La comparacion decide EN QUE DOMINIO se razona, que no tiene por que ser
     * el del tipo declarado: un `ult` sobre un `i32` compara sin signo.  Los dos
     * operandos se releen en ese dominio, se restringe alli, y el resultado se
     * vuelve a leer en el tipo del valor.  Cuando alguna de esas relecturas no
     * es monotona el dominio responde "todo", y entonces no se afirma nada --
     * que es exactamente lo que hay que hacer.
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
        if (!ra.acotada() || !rb.acotada()) return;
        if (ra.t.bits != rb.t.bits) return; // el IR no deberia comparar anchos distintos

        const bool sin_signo = (o == IrOp::CMP_ULT || o == IrOp::CMP_ULE ||
                                o == IrOp::CMP_UGT || o == IrOp::CMP_UGE);
        const RangeType dc = RangeType::de(ra.t.bits, sin_signo);
        const ValueRange ca = ra.reinterpretar(dc), cb = rb.reinterpretar(dc);

        // Devuelve el valor restringido, ya releido en el tipo del propio valor.
        auto aplicar = [&](ir::IrValueId v, const ValueRange &orig,
                           const ValueRange &restringido) {
            if (!e.alcanzable) return;
            if (v == ir::IR_NO_VALUE || v >= suelo.size()) return;
            if (restringido.es_bottom()) { e.inalcanzable(); return; }
            const ValueRange nuevo = orig.cortar(restringido.reinterpretar(orig.t));
            if (nuevo.es_bottom()) { e.inalcanzable(); return; }
            if (!nuevo.es_top()) e.poner(v, nuevo);
        };

        switch (o) {
        case IrOp::CMP_LT: case IrOp::CMP_ULT:
            aplicar(va, ra, ca.restringir_menor(cb));
            aplicar(vb, rb, cb.restringir_mayor(ca));
            break;
        case IrOp::CMP_LE: case IrOp::CMP_ULE:
            aplicar(va, ra, ca.restringir_menor_igual(cb));
            aplicar(vb, rb, cb.restringir_mayor_igual(ca));
            break;
        case IrOp::CMP_GT: case IrOp::CMP_UGT:
            aplicar(va, ra, ca.restringir_mayor(cb));
            aplicar(vb, rb, cb.restringir_menor(ca));
            break;
        case IrOp::CMP_GE: case IrOp::CMP_UGE:
            aplicar(va, ra, ca.restringir_mayor_igual(cb));
            aplicar(vb, rb, cb.restringir_menor_igual(ca));
            break;
        case IrOp::CMP_EQ:
            aplicar(va, ra, ca.restringir_igual(cb));
            aplicar(vb, rb, cb.restringir_igual(ca));
            break;
        case IrOp::CMP_NE:
            aplicar(va, ra, ca.restringir_distinto(cb));
            aplicar(vb, rb, cb.restringir_distinto(ca));
            break;
        default:
            break;
        }
    }

    /**
     * @brief Lo que afirma el BRAZO de un switch sobre su selector.
     *
     * En un brazo concreto el tag vale exactamente uno; en el brazo por defecto,
     * cualquier cosa menos los de la tabla.  Lo segundo solo se puede decir con
     * un intervalo cuando la tabla toca un extremo del tipo -- si la muerde por
     * en medio quedarian dos trozos --, y el dominio ya sabe distinguirlo.
     */
    void estrechar_por_caso(Estado &e, const Arista &a) const {
        if (!e.alcanzable) return;
        if (a.sel == ir::IR_NO_VALUE || a.sel >= suelo.size()) return;
        const ValueRange orig = valor(e, a.sel);
        if (!orig.acotada()) return;
        const ValueRange caso = ValueRange::crudo(orig.t, a.caso_lo, a.caso_hi);
        if (!caso.acotada()) return; // la tabla no cabe en el tipo del selector
        const ValueRange nuevo =
            a.dentro ? orig.restringir_igual(caso) : orig.restringir_fuera(caso);
        if (nuevo.es_bottom()) { e.inalcanzable(); return; }
        if (!nuevo.es_top()) e.poner(a.sel, nuevo);
    }

    // --- transferencia: la pone Contexto, compartida con la consulta --------

    // --- ecuaciones ---------------------------------------------------------
    Estado calcular_in(ir::IrBlockId bi) const {
        Estado e;
        for (uint32_t ai : entrantes[bi])
            if (out_arista[ai].alcanzable) e = unir_estados(e, out_arista[ai]);
        if (bi == 0) e.alcanzable = true; // a la entrada siempre se llega
        if (!e.alcanzable) return e;
        resolver_phis(bi, e);
        return e;
    }

    /**
     * @brief Cada PHI vale la union de lo que trae cada arista, leido EN ella.
     *
     * Es lo que hace el analisis sensible al flujo y no una propagacion global:
     * sin leer el estado DE LA ARISTA, una guarda no puede afectar a la PHI que
     * depende de ella.
     *
     * Si dos aristas vienen del MISMO bloque (un `switch` con dos casos al mismo
     * destino), las dos aportan y se unen: el argumento de la PHI identifica el
     * bloque de origen, no la arista, y unir de mas es correcto.
     */
    void resolver_phis(ir::IrBlockId bi, Estado &e) const {
        for (const ir::IrInstr &in : fn.blocks[bi].instrs) {
            if (in.op != IrOp::PHI) break; // van al principio del bloque
            if (in.dst == ir::IR_NO_VALUE || in.dst >= suelo.size()) continue;
            ValueRange acc = ValueRange::bottom(suelo[in.dst].t);
            for (const ir::IrPhiArg &pa : in.phi_args)
                for (uint32_t ai : entrantes[bi])
                    if (aristas[ai].desde == pa.block && out_arista[ai].alcanzable)
                        acc = acc.unir(valor(out_arista[ai], pa.value));
            e.poner(in.dst, encajar_en(acc, suelo[in.dst]));
        }
    }

    Estado calcular_out(ir::IrBlockId bi, const Estado &in) const {
        Estado e = in;
        for (const ir::IrInstr &instr : fn.blocks[bi].instrs)
            if (instr.op != IrOp::PHI) transferir(instr, e);
        return e;
    }

    /// Recalcula las aristas de salida y encola los destinos que cambiaron.
    void propagar(ir::IrBlockId bi, std::deque<ir::IrBlockId> &cola) {
        const Estado out = calcular_out(bi, in_bloque[bi]);
        for (uint32_t ai : salientes[bi]) {
            Estado se = out;
            if (aristas[ai].cond != ir::IR_NO_VALUE)
                estrechar_por_guarda(se, aristas[ai].cond, aristas[ai].rama);
            if (aristas[ai].sel != ir::IR_NO_VALUE)
                estrechar_por_caso(se, aristas[ai]);
            if (!(se == out_arista[ai])) {
                out_arista[ai] = std::move(se);
                cola.push_back(aristas[ai].hasta);
            }
        }
    }

    /// Alguna arista de retroceso que llega aqui esta viva: el bloque cierra un
    /// ciclo por el que ya se ha vuelto a pasar.
    bool cierra_ciclo(ir::IrBlockId bi) const {
        for (uint32_t ai : entrantes[bi])
            if (aristas[ai].retroceso && out_arista[ai].alcanzable) return true;
        return false;
    }

    /**
     * @brief ASCENSO: crece hasta un post-punto-fijo.  Ensancha en los ciclos.
     * @return true si la lista de trabajo se vacio sola.
     */
    bool resolver_ascenso(int presupuesto) {
        std::deque<ir::IrBlockId> cola;
        cola.push_back(0);
        int pasos = 0;
        while (!cola.empty()) {
            if (++pasos > presupuesto) return false;
            stats.pasos++;
            const ir::IrBlockId bi = cola.front();
            cola.pop_front();

            Estado nuevo_in = calcular_in(bi);
            if (cierra_ciclo(bi) && vueltas_ciclo[bi] >= op.retardo_ensanche) {
                nuevo_in = ensanchar_estado(in_bloque[bi], nuevo_in);
                stats.ensanches++;
            }
            if (!(nuevo_in == in_bloque[bi])) {
                in_bloque[bi] = std::move(nuevo_in);
                vueltas_ciclo[bi]++;
                stats.cambios++;
            }
            if (!in_bloque[bi].alcanzable) continue;
            propagar(bi, cola);
        }
        return true;
    }

    /**
     * @brief DESCENSO: parte de la solucion ensanchada y SOLO estrecha.
     *
     * No es el ascenso con una bandera: aqui no se ensancha nunca y cada IN
     * nuevo se cruza con el anterior, asi que la sucesion es decreciente.  Eso
     * es lo que recupera la precision que el ensanchamiento solto -- en
     * `for (i = 0; i < 200)` el ascenso deja `[0, max]` y el descenso lo devuelve
     * a `[0,199]` -- sin arriesgar la terminacion: un presupuesto agotado aqui
     * cuesta precision, jamas correccion.
     */
    bool resolver_descenso(int presupuesto) {
        std::deque<ir::IrBlockId> cola;
        for (uint32_t bi = 0; bi < fn.blocks.size(); ++bi) cola.push_back(bi);
        int pasos = 0;
        while (!cola.empty()) {
            if (++pasos > presupuesto) return false;
            stats.pasos++;
            const ir::IrBlockId bi = cola.front();
            cola.pop_front();

            const Estado nuevo_in = estrechar_estado(in_bloque[bi], calcular_in(bi));
            if (!(nuevo_in == in_bloque[bi])) {
                in_bloque[bi] = nuevo_in;
                stats.estrechados++;
            }
            if (!in_bloque[bi].alcanzable) continue;
            propagar(bi, cola);
        }
        return true;
    }

    /// El estado de entrada de cada bloque, para que se pueda preguntar por un
    /// PUNTO despues de que el motor haya terminado.
    std::vector<RangeBlockState> estados_de_entrada() const {
        std::vector<RangeBlockState> out(fn.blocks.size());
        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            out[bi].alcanzable = in_bloque[bi].alcanzable;
            out[bi].refinamientos = in_bloque[bi].ref;
        }
        return out;
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

void Contexto::transferir(const ir::IrInstr &in, Estado &e) const {
        if (!e.alcanzable) return;
        if (in.dst == ir::IR_NO_VALUE || in.dst >= suelo.size()) return;
        const ValueRange piso = suelo[in.dst];
        auto arg = [&](size_t i) {
            return i < in.operands.size() ? valor(e, in.operands[i])
                                          : ValueRange::top();
        };
        ValueRange nuevo = ValueRange::top(piso.t);
        switch (in.op) {
        case IrOp::CONST:
            nuevo = piso.acotada() ? ValueRange::constante(piso.t, in.imm)
                                   : ValueRange::top();
            break;
        case IrOp::MOV: nuevo = arg(0); break;
        case IrOp::ADD: nuevo = arg(0).sumar(arg(1)); break;
        case IrOp::SUB: nuevo = arg(0).restar(arg(1)); break;
        case IrOp::MUL: nuevo = arg(0).multiplicar(arg(1)); break;
        case IrOp::NEG: nuevo = arg(0).negar(); break;
        case IrOp::DIV: nuevo = arg(0).dividir(arg(1)); break;
        case IrOp::MOD: nuevo = arg(0).resto(arg(1)); break;
        case IrOp::AND: nuevo = arg(0).conjuncion(arg(1)); break;
        case IrOp::OR: nuevo = arg(0).disyuncion(arg(1)); break;
        case IrOp::XOR: nuevo = arg(0).exclusiva(arg(1)); break;
        case IrOp::NOT: nuevo = arg(0).complemento(); break;
        case IrOp::SHL: nuevo = arg(0).desplazar_izq(arg(1)); break;
        case IrOp::SHR: nuevo = arg(0).desplazar_der_logico(arg(1)); break;
        case IrOp::SAR: nuevo = arg(0).desplazar_der_aritmetico(arg(1)); break;
        case IrOp::SEXT:
            if (piso.acotada()) nuevo = arg(0).extender_con_signo(piso.t);
            break;
        case IrOp::ZEXT:
            if (piso.acotada()) nuevo = arg(0).extender_sin_signo(piso.t);
            break;
        case IrOp::TRUNC:
            if (piso.acotada()) nuevo = arg(0).truncar(piso.t);
            break;
        /* BITCAST reinterpreta BITS.  Entre un float y un entero el rango del
         * origen no dice nada del destino; entre dos enteros del mismo ancho los
         * bits SON el valor -- que es como el IR pasa un indice `u64` a una suma
         * de punteros, y tirarlo dejaba fuera de comprobacion justo los accesos
         * indexados.  El dominio decide cual de los dos casos es. */
        case IrOp::BITCAST:
            if (piso.acotada()) nuevo = arg(0).reinterpretar(piso.t);
            break;
        /* El resultado de una llamada no es desconocido si se puede leer el
         * cuerpo de quien la atiende: lo que devuelve sale de SU codigo y vale
         * para cualquier llamante, se conozcan o no los demas. */
        case IrOp::CALL:
        case IrOp::CALLIND:
            if (sum.hay() && piso.acotada()) {
                const std::string destino =
                    (in.op == IrOp::CALL)
                        ? in.func_name
                        : funcion_apuntada(fn, facts, in.func_ptr);
                if (const FnRangeSummary *s = sum.buscar(destino))
                    nuevo = s->ret;
            }
            break;
        default:
            break; // op sin modelar: lo que diga el tipo
        }
        e.poner(in.dst, encajar_en(nuevo, piso));
}

} // namespace

uint64_t huella_de_funcion(const ir::IrFunction &fn) {
    uint64_t h = util::kFnvOffset;
    for (const ir::IrBlock &b : fn.blocks) {
        h = util::fnv_mix(h, b.instrs.size());
        for (const ir::IrInstr &in : b.instrs) {
            h = util::fnv_mix(h, static_cast<uint64_t>(in.op));
            // El TIPO va dentro: el suelo de cada rango sale de el, no de la
            // forma de la instruccion.
            h = util::fnv_bytes(h, &in.type, sizeof(in.type));
            h = util::fnv_mix(h, in.dst);
            h = util::fnv_mix(h, in.target_block);
            h = util::fnv_mix(h, in.false_block);
            h = util::fnv_bytes(h, in.operands.data(),
                                in.operands.size() * sizeof(ir::IrValueId));
            h = util::fnv_bytes(h, in.func_name.data(), in.func_name.size());
            h = util::fnv_mix(h, in.func_ptr); // el destino de una llamada INDIRECTA
            for (const ir::IrPhiArg &pa : in.phi_args) {
                h = util::fnv_mix(h, pa.value);
                h = util::fnv_mix(h, pa.block);
            }
        }
    }
    // Los valores constantes y sus tipos son el suelo de todo el analisis.
    for (const auto &v : fn.values) {
        h = util::fnv_bytes(h, &v.type, sizeof(v.type));
        h = util::fnv_mix(h, v.is_const ? v.const_val : 0);
        h = util::fnv_mix(h, v.is_const ? 1u : 0u);
    }
    for (const ir::IrValueId p : fn.params) h = util::fnv_mix(h, p);
    return h;
}

/**
 * @brief Mezcla un rango en una huella, CAMPO A CAMPO.
 *
 * Nunca por bytes: `ValueRange` tiene 5 bytes de relleno de alineacion entre
 * `sin_signo` y `lo_c` que nadie inicializa, asi que dos rangos identicos
 * pueden dar huellas distintas segun lo que hubiera antes en esa memoria.  Una
 * huella que depende de basura no sirve para decidir si algo se puede reusar.
 */
static uint64_t mezcla_rango(uint64_t h, const ValueRange &r) {
    h = util::fnv_mix(h, static_cast<uint64_t>(r.kind));
    h = util::fnv_mix(h, r.t.bits);
    h = util::fnv_mix(h, r.t.sin_signo ? 1u : 0u);
    h = util::fnv_mix(h, r.lo_c);
    h = util::fnv_mix(h, r.hi_c);
    return h;
}

uint64_t huella_de_resumen(const FnRangeSummary *s) {
    // "No hay resumen" es un estado propio: si manana lo hay, el resultado
    // puede cambiar, asi que no puede colisionar con un resumen vacio.
    if (s == nullptr) return util::fnv_mix(util::kFnvOffset, 0xFFFFFFFFFFFFFFFFull);
    uint64_t h = util::fnv_mix(util::kFnvOffset, s->cerrada ? 1u : 0u);
    for (const ValueRange &p : s->params) h = mezcla_rango(h, p);
    return mezcla_rango(h, s->ret);
}

bool dependencias_vigentes(const DependenciasRango &d, const ir::IrFunction &fn,
                           const RangeOptions &op, const RangeSummaries *sum) {
    // Sin registro no se afirma nada: se recalcula.  Es la misma regla que rige
    // el resto del analisis -- no haber mirado no es haber comprobado.
    if (!d.registrada) return false;
    if (d.huella_opciones != util::fnv_bytes(util::kFnvOffset, &op, sizeof(op)))
        return false;
    if (d.huella_ir != huella_de_funcion(fn)) return false;
    // Se RELEE cada resumen que se consulto, contra el estado de ahora.
    for (const auto &leida : d.resumenes) {
        const FnRangeSummary *s = sum ? sum->buscar(leida.first) : nullptr;
        if (huella_de_resumen(s) != leida.second) return false;
    }
    return true;
}

/// Tiempo total dentro del motor, para contrastarlo con lo que diga un perfil:
/// una lista de puntos calientes dice DONDE, no CUANTO, y confundirlo ya ha
/// costado varias hipotesis en falso.
static std::atomic<long long> g_ns_motor{0};
static std::atomic<long long> g_n_motor{0};

static RangeFacts calcular_rangos_impl(const ir::IrFunction &fn, const IrFacts &facts,
                                       const RangeOptions &op, const RangeSummaries *sum);

static RangeFacts calcular_rangos(const ir::IrFunction &fn, const IrFacts &facts,
                                  const RangeOptions &op, const RangeSummaries *sum) {
    const uint64_t t = util::reloj::ahora();
    RangeFacts r = calcular_rangos_impl(fn, facts, op, sum);
    g_ns_motor += util::reloj::a_ns(util::reloj::ahora() - t);
    if ((++g_n_motor % 100) == 0 && std::getenv("VESTA_RANGE_STATS"))
        std::fprintf(stderr, "[motor-rangos] %lld analisis | %lld ms\n",
                     g_n_motor.load(), g_ns_motor.load() / 1000000);
    return r;
}

static RangeFacts calcular_rangos_impl(const ir::IrFunction &fn, const IrFacts &facts,
                                       const RangeOptions &op, const RangeSummaries *sum) {
    /* --------------------------------------------------------------- reuso
     *
     * Siete sitios distintos piden rangos de la misma funcion, y medido sobre un
     * programa real el 75 % de las peticiones son REPETICIONES exactas: misma
     * funcion, mismas opciones, mismos resumenes.  Recalcularlas no cambia nada.
     *
     * El indice va por la parte de la clave que se puede calcular SIN correr el
     * analisis (funcion + opciones); lo que solo se sabe despues -- que
     * resumenes se consultaron -- se comprueba releyendolos, que es justo lo que
     * hace @c dependencias_vigentes.  Por eso un cajon puede tener mas de una
     * entrada: mismo codigo, distinto entorno.
     *
     * Es una cache, no un buffer reaprovechado: se indexa por la entrada, no se
     * pisa mientras vale, y varios hilos pueden leer la misma. */
    RangeFacts out;
    g_coste = CosteEstado{}; // el coste que se mide es el de ESTA funcion
    Motor m(fn, facts, op, sum);
    if (fn.blocks.empty()) {
        out.r = m.suelo;
        return out;
    }
    const int presupuesto =
        static_cast<int>(op.pasos_por_bloque * fn.blocks.size() + op.pasos_extra);

    const bool ok = m.resolver_ascenso(presupuesto);
    if (ok) {
        const int tope_descenso =
            static_cast<int>(op.pasos_descenso * fn.blocks.size() + op.pasos_extra);
        /* Un descenso a medias sigue siendo correcto: toda la cadena
         * descendente arranca de un post-punto-fijo y solo estrecha, asi que
         * cualquier parada intermedia sigue conteniendo al punto fijo real.
         * Por eso no tumba la convergencia; solo se anota. */
        m.stats.descenso_completo = m.resolver_descenso(tope_descenso);
    }

    out.stats = m.stats;
    out.convergio = ok;
    if (!out.convergio) {
        // Sin punto fijo no hay hecho que sostener: no se afirma nada.
        out.r.assign(facts.def_of.size(), ValueRange::top());
        return out;
    }
    out.r = m.en_definicion();
    out.entrada = m.estados_de_entrada();

    /* Lo que se leyo para llegar aqui.  No se enumera: se recoge de lo que el
     * lector fue apuntando, asi que incluye lo que se consulto de verdad --
     * incluidas las llamadas indirectas, que no llevan el nombre escrito. */
    out.deps.huella_ir = huella_de_funcion(fn);
    /* Los HECHOS (def-use) NO entran aqui, y no por descuido: se derivan de la
     * funcion, asi que la huella de la funcion ya los cubre.  Mezclarlos
     * ademas rompia el invariante que sostiene todo esto -- que
     * `dependencias_vigentes` pueda RECALCULAR esta misma huella para
     * comprobarla --, porque alli solo se tiene la funcion.  El resultado era
     * una comparacion que no coincidia nunca: la cache no acertaba jamas.
     * Medido en su momento: anadirlos no quitaba ni un caso incoherente. */
    out.deps.huella_opciones = util::fnv_bytes(util::kFnvOffset, &op, sizeof(op));
    out.deps.resumenes = m.sum.soltar();
    out.deps.registrada = true;


    // Densidad: cuantos de los valores de la funcion acaban teniendo rango en
    // un estado.  Es lo que decide si conviene guardar el estado DISPERSO (como
    // ahora) o DENSO indexado por valor; suponerlo seria elegir a ciegas.
    out.stats.valores = static_cast<uint32_t>(m.suelo.size());
    for (const RangeBlockState &e : out.entrada) {
        if (!e.alcanzable) continue;
        const uint32_t n = static_cast<uint32_t>(e.refinamientos.size());
        if (n > out.stats.ref_max) out.stats.ref_max = n;
        out.stats.ref_suma += n;
        ++out.stats.ref_muestras;
    }
    out.stats.inserciones = g_coste.inserciones;
    out.stats.reescrituras = g_coste.reescrituras;
    out.stats.copias = g_coste.copias;
    out.stats.busquedas = g_coste.busquedas;
    out.stats.uniones = g_coste.uniones;
    out.stats.unidos = g_coste.unidos;

    if (const char *v = std::getenv("VESTA_RANGE_STATS")) {
        if (v[0] == '1') {
            /* Rehacer un analisis no es lo mismo que hacer trabajo: si el
             * resultado sale IGUAL que la vez anterior, la vuelta entera sobro y
             * lo que hay que arreglar es la invalidacion, no la estructura.  Se
             * compara por huella del resultado, no por tiempo. */
            uint64_t h = util::kFnvOffset;
            for (const ValueRange &r : out.r) h = mezcla_rango(h, r);
            for (const RangeBlockState &e : out.entrada) {
                h = util::fnv_mix(h, e.alcanzable ? 1u : 0u);
                for (const auto &p : e.refinamientos) {
                    h = util::fnv_mix(h, p.first);
                    h = mezcla_rango(h, p.second);
                }
            }
            /* Y la otra mitad de la pregunta: si sale igual porque la ENTRADA
             * no habia cambiado, entonces lo que sobra es recalcular, no la
             * estructura.  Se usa el registro que dejo el propio analisis -- no
             * una huella calculada aparte: dos criterios acaban discrepando. */
            uint64_t hir = util::fnv_mix(out.deps.huella_ir, out.deps.huella_opciones);
            for (const auto &leida : out.deps.resumenes) {
                hir = util::fnv_bytes(hir, leida.first.data(), leida.first.size());
                hir = util::fnv_mix(hir, leida.second);
            }
            /* La comprobacion va por CLAVE, no "contra la vez anterior": el orden
             * de las llamadas cambia entre corridas (se compila en paralelo), y
             * comparar contra la anterior daba numeros distintos cada vez.  Asi
             * la pregunta es la correcta y no depende del orden: la MISMA clave,
             * ha dado alguna vez DOS resultados distintos?  Si ocurre una sola
             * vez, la clave esta incompleta y reusar serviria un valor viejo. */
            static std::mutex mx;
            static std::unordered_map<uint64_t, uint64_t> por_clave;
            bool visto, incoherente;
            {
                std::lock_guard<std::mutex> g(mx);
                auto it = por_clave.find(hir);
                visto = (it != por_clave.end());
                incoherente = (visto && it->second != h);
                if (!visto) por_clave.emplace(hir, h);
            }
            std::fprintf(stderr, "[rangos] visto=%d incoherente=%d ", visto ? 1 : 0,
                         incoherente ? 1 : 0);
            std::fprintf(stderr,
                         "%s valores=%u bloques=%zu ref_max=%u "
                         "ref_media=%.1f altas=%llu reescrituras=%llu copias=%llu "
                         "busquedas=%llu uniones=%llu unidos=%llu\n",
                         fn.name.c_str(), out.stats.valores, fn.blocks.size(),
                         out.stats.ref_max,
                         out.stats.ref_muestras
                             ? double(out.stats.ref_suma) / out.stats.ref_muestras
                             : 0.0,
                         (unsigned long long)out.stats.inserciones,
                         (unsigned long long)out.stats.reescrituras,
                         (unsigned long long)out.stats.copias,
                         (unsigned long long)out.stats.busquedas,
                         (unsigned long long)out.stats.uniones,
                         (unsigned long long)out.stats.unidos);
        }
    }
    return out;
}

// ===========================================================================
//  Reuso
// ===========================================================================

/**
 * @brief Los rangos de una funcion, calculados o reusados, SIN copiarlos.
 *
 * Devolver por valor era el problema: `RangeFacts` lleva dentro el estado de
 * entrada de CADA bloque, asi que un acierto de cache copiaba todo eso.  Medido
 * en el camino del asm -- que pide los rangos de la funcion entera una vez por
 * bloque de asm --, esa copia costaba 16 s de una compilacion de 26.
 *
 * Se devuelve un puntero COMPARTIDO, no una referencia al cajon: asi la entrada
 * puede desalojarse sin dejar colgado a quien la estaba mirando.
 */
static std::shared_ptr<const RangeFacts>
rangos_de(const ir::IrFunction &fn, const IrFacts &facts, const RangeOptions &op,
          const RangeSummaries *sum) {
    struct EntradaCache {
        DependenciasRango                 deps;
        std::shared_ptr<const RangeFacts> hechos;
    };
    static std::mutex mx_cache;
    static std::unordered_map<uint64_t, std::vector<EntradaCache>> cache;

    if (std::getenv("VESTA_NO_RANGE_CACHE") != nullptr)
        return std::make_shared<const RangeFacts>(calcular_rangos(fn, facts, op, sum));

    /* El indice va por la parte de la clave que se puede calcular SIN correr el
     * analisis (funcion + opciones); lo que solo se sabe despues -- que
     * resumenes se consultaron -- se comprueba releyendolos.  Por eso un cajon
     * puede tener varias entradas: mismo codigo, distinto entorno. */
    const uint64_t clave =
        util::fnv_mix(huella_de_funcion(fn),
                      util::fnv_bytes(util::kFnvOffset, &op, sizeof(op)));
    {
        std::lock_guard<std::mutex> g(mx_cache);
        auto it = cache.find(clave);
        if (it != cache.end())
            for (const EntradaCache &e : it->second)
                if (dependencias_vigentes(e.deps, fn, op, sum)) return e.hechos;
    }

    auto nuevos = std::make_shared<const RangeFacts>(calcular_rangos(fn, facts, op, sum));
    {
        std::lock_guard<std::mutex> g(mx_cache);
        std::vector<EntradaCache> &cajon = cache[clave];
        // Tope por cajon: mismo codigo con muchos entornos no puede crecer sin fin.
        if (cajon.size() >= 8) cajon.erase(cajon.begin());
        cajon.push_back(EntradaCache{nuevos->deps, nuevos});
    }
    return nuevos;
}

std::shared_ptr<const RangeFacts> compute_ranges_ptr(const ir::IrFunction &fn,
                                                     const IrFacts &facts,
                                                     const RangeOptions &op,
                                                     const RangeSummaries *sum) {
    return rangos_de(fn, facts, op, sum);
}

RangeFacts compute_ranges(const ir::IrFunction &fn, const IrFacts &facts,
                          const RangeOptions &op, const RangeSummaries *sum) {
    // Quien solo va a LEERLOS deberia usar `compute_ranges_ptr` y ahorrarse esta
    // copia; esta forma se mantiene para quien necesite los suyos propios.
    return *rangos_de(fn, facts, op, sum);
}

// ===========================================================================
//  Consulta por punto
// ===========================================================================

struct RangeWalk::Impl {
    Contexto ctx;
    const RangeFacts &rf;
    const ir::IrBlock *bloque = nullptr;
    Estado estado;
    size_t idx = 0;

    Impl(const ir::IrFunction &fn, const IrFacts &facts, const RangeFacts &r,
         ir::IrBlockId b)
        : ctx(fn, facts, nullptr), rf(r) {
        if (b >= fn.blocks.size()) return;
        bloque = &fn.blocks[b];
        if (b < rf.entrada.size()) {
            estado.alcanzable = rf.entrada[b].alcanzable;
            estado.ref = rf.entrada[b].refinamientos;
        } else {
            /* Sin estado guardado -- rangos que no convergieron, o un bloque
             * anadido despues -- no se puede afirmar por punto, pero tampoco hay
             * que mentir: se responde lo que diga la definicion. */
            estado.alcanzable = true;
        }
    }
};

RangeWalk::RangeWalk(const ir::IrFunction &fn, const IrFacts &facts,
                     const RangeFacts &rf, ir::IrBlockId b)
    : impl_(new Impl(fn, facts, rf, b)) {}

RangeWalk::RangeWalk(RangeWalk &&o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }

RangeWalk::~RangeWalk() { delete impl_; }

bool RangeWalk::alcanzable() const {
    return impl_ != nullptr && impl_->estado.alcanzable;
}

ValueRange RangeWalk::rango(ir::IrValueId v) const {
    if (impl_ == nullptr) return ValueRange::top();
    const ValueRange en_def = impl_->rf.at(v);
    const ValueRange en_punto = impl_->ctx.valor(impl_->estado, v);
    /* Los dos son ciertos aqui: la definicion domina al uso, y el estado del
     * punto lleva lo que las guardas afirmaron por el camino.  Si el corte
     * quedara vacio seria una incoherencia del propio motor, y ante eso se
     * responde lo conocido en vez de declarar el punto imposible. */
    const ValueRange c = en_punto.cortar(en_def);
    return c.es_bottom() ? en_def : c;
}

void RangeWalk::avanzar() {
    if (impl_ == nullptr || impl_->bloque == nullptr) return;
    if (impl_->idx >= impl_->bloque->instrs.size()) return;
    const ir::IrInstr &in = impl_->bloque->instrs[impl_->idx++];
    /* Las PHI no se reproducen: su valor lo fijo el motor leyendo el estado de
     * cada ARISTA entrante, y eso ya viene resuelto en el estado de entrada. */
    if (in.op != ir::IrOp::PHI) impl_->ctx.transferir(in, impl_->estado);
}

} // namespace analysis
