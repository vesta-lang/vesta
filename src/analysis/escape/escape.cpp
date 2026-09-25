/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file escape.cpp
 * @brief Implementacion de EscapeAnalysis: COMPLETA (cada op modelado, sin
 * bail)
 *        + INTERPROCEDURAL (los CALL resuelven captura via el summary del
 *        callee, cerrado por punto-fijo).
 */
// De que modulo es este directorio.  Ver
// `src/analysis/asa/aggregate_facts.cpp`.
#include "util/report/alloc_csv_c.h"

VESTA_ALLOC_MODULE_HERE("analysis");

#include "analysis/escape/escape.h"

#include "ir/ir_vec_ops.h" // cuales son las operaciones vectoriales
#include "ir/ssa_ir.h"
#include "util/named_alloc.h"
#include "util/scc.h" // el grafo de llamadas, por componentes

#include <deque>
#include <string_view>

namespace analysis {

char EscapeAnalysis::ID = 0;

namespace scratch {
struct EscapeCallOffsets;  ///< Funcion -> donde empiezan sus llamadas.
struct EscapeCallTargets;  ///< A quien llama cada llamada.
struct EscapeComp;         ///< Funcion -> su componente fuertemente conexa.
struct EscapeCompOffsets;  ///< Componente -> donde empiezan sus miembros.
struct EscapeCompMembers;  ///< Los miembros de cada componente.
struct EscapeNodePos;      ///< Nodo del grafo -> posicion en el modulo.
} // namespace scratch

const EscapeInfo *ModuleEscape::find(const ir::IrModule &mod,
                                     const ir::IrFunction &fn) const {
    /* Si @p fn no es de este modulo, no hay respuesta que dar. */
    const ir::IrFnPos pos = mod.pos_of(fn);
    if (pos == ir::IR_NO_FN || pos >= by_fn.size() ||
        state[pos] != EscapeFnState::ANALYZED)
        return nullptr;
    return &by_fn[pos];
}

namespace {

using ir::IrOp;

// ¿La posicion (op, idx) usa el operando como DIRECCION de memoria
// (leer/escribir su CONTENIDO, sin capturar el puntero)?  Lista COMPLETA de
// accesos a memoria.
bool is_address_operand(IrOp op, size_t idx) {
    // Ops VECTORIALES: todos sus operandos-puntero son DIRECCIONES (leen/
    // escriben memoria, no capturan el puntero), asi que la posicion da igual.
    // Se resuelven antes del switch para no repetir la lista: enumerarlas a
    // mano ya dejo fuera VEC_FMA_S, y una operacion que falte aqui cae al
    // `default` -- se da su puntero por capturado y el objeto escapa, que es
    // el lado seguro, pero pierde la optimizacion sin decirlo.
    if (is_vec_op(op)) {
        (void)idx;
        return true;
    }
    switch (op) {
    case IrOp::LOAD: return idx == 0;
    case IrOp::STORE: return idx == 1; // [0]=valor (captura), [1]=addr
    case IrOp::GETFIELD: return idx == 0;
    case IrOp::SETFIELD: return idx == 0; // base; el valor es captura
    case IrOp::GCWB_IR: return idx == 0;
    case IrOp::ARRAY_LOAD: return idx == 0;
    case IrOp::ARRAY_STORE: return idx == 0; // base; index/valor no-address
    case IrOp::ARRAY_LEN: return idx == 0;
    case IrOp::STRLEN:
    case IrOp::STRGETBYTES:
    case IrOp::STRHASH: return idx == 0;
    case IrOp::MEMCPY: return idx == 0 || idx == 1; // dst, src (contenido)
    case IrOp::MEMSET: return idx == 0; // dst (contenido); val es escalar
    default: return false;
    }
}

// ¿op es una DERIVACION de puntero (su resultado es un puntero derivado cuya
// raiz decide points_to)?  Se resuelve TRANSITIVAMENTE: el operando no captura
// si el resultado sigue siendo la MISMA raiz.
bool is_derivation(IrOp op) {
    switch (op) {
    case IrOp::GEP:
    case IrOp::ADD:
    case IrOp::SUB:
    case IrOp::BITCAST:
    case IrOp::CAST:
    // Un prestamo REENVIA el puntero, asi que si el prestamo se escapa, se
    // escapa lo que se presto.  Su segundo operando es el dueno, y por eso
    // tambien queda alcanzado: es justo lo que hay que saber.
    case IrOp::BORROW:
    case IrOp::MOV:
    case IrOp::GCDEREF_IR:
    case IrOp::GC_DEREF_HOST:
    case IrOp::GC_HANDLE_FOR_PTR:
    case IrOp::UNWRAP:
    case IrOp::MVTAKE_IR: return true;
    default: return false;
    }
}

// ¿op LEE el valor del puntero sin capturarlo (comparacion / test)?  No escapa.
bool is_comparison(IrOp op) {
    switch (op) {
    case IrOp::CMP_EQ:
    case IrOp::CMP_NE:
    case IrOp::CMP_LT:
    case IrOp::CMP_GT:
    case IrOp::CMP_LE:
    case IrOp::CMP_GE:
    case IrOp::CMP_ULT:
    case IrOp::CMP_UGT:
    case IrOp::CMP_ULE:
    case IrOp::CMP_UGE:
    case IrOp::FCMP_EQ:
    case IrOp::FCMP_NE:
    case IrOp::FCMP_LT:
    case IrOp::FCMP_GT:
    case IrOp::FCMP_LE:
    case IrOp::FCMP_GE:
    case IrOp::ISNULL:
    case IrOp::INSTANCEOF: return true;
    default: return false;
    }
}

// ¿op es una llamada ESTATICA (callee conocido por nombre) cuyos args resuelven
// captura via el summary del callee?
bool is_static_call(IrOp op) {
    return op == IrOp::CALL || op == IrOp::TAILCALL;
}
// ¿op es una llamada DINAMICA/nativa (callee desconocido -> captura todos)?
bool is_dynamic_call(IrOp op) {
    switch (op) {
    case IrOp::CALLVIRT:
    case IrOp::CALLM:
    case IrOp::CALLITF:
    case IrOp::CALLIND:
    case IrOp::CALLCLOSURE:
    case IrOp::CALLN:
    case IrOp::CALLSUPER: return true;
    default: return false;
    }
}

/// Las raices de las que se sabe algo, y donde se apunta que escapan.
struct EscapeMarks {
    const PointsTo &pt;
    EscapeInfo &out;

    /// Marca la raiz de @p v (reserva de pila o parametro) como escapante.
    void escape(ir::IrValueId v) {
        const PointsToEntry &e = pt.at(v);
        if (e.kind == effects::AbstractLoc::Kind::Stack &&
            e.root != effects::LOC_GENERIC)
            out.escaping_stack.insert(e.root);
        else if (e.kind == effects::AbstractLoc::Kind::ArgDerived)
            out.escaping_params.insert(static_cast<int32_t>(e.root));
    }

    /// @p v resuelve a una raiz SEGUIDA: reserva de pila local o parametro.
    bool tracked_root(ir::IrValueId v) const {
        const PointsToEntry &e = pt.at(v);
        return (e.kind == effects::AbstractLoc::Kind::Stack &&
                e.root != effects::LOC_GENERIC) ||
               e.kind == effects::AbstractLoc::Kind::ArgDerived;
    }

    /// @p a y @p b tienen la misma raiz conocida.
    bool same_root(ir::IrValueId a, ir::IrValueId b) const {
        const PointsToEntry &ea = pt.at(a), &eb = pt.at(b);
        return ea.kind == eb.kind && ea.root == eb.root &&
               ea.kind != effects::AbstractLoc::Kind::Unknown;
    }
};

} // namespace

EscapeInfo compute_escape(const ir::IrFunction &fn, const IrFacts &facts,
                          const PointsTo &pt,
                          const CalleeEscapesParam &callee) {
    (void)facts;
    EscapeInfo out;
    EscapeMarks marks{pt, out};

    for (const ir::IrBlock &b : fn.blocks) {
        for (const ir::IrInstr &ins : b.instrs) {
            for (size_t p = 0; p < ins.operands.size(); ++p) {
                const ir::IrValueId o = ins.operands[p];
                if (o == ir::IR_NO_VALUE) continue;
                if (!marks.tracked_root(o)) continue; // solo alloca/param

                // 1) DIRECCION de memoria: lee/escribe contenido, no captura.
                if (is_address_operand(ins.op, p)) continue;
                // 2) COMPARACION: lee el valor, no captura.
                if (is_comparison(ins.op)) continue;
                // 3) DERIVACION: no captura SI el resultado sigue siendo la
                //    MISMA raiz (se decidira en los usos del resultado); si la
                //    derivacion PIERDE la raiz (dst Unknown), la direccion fue
                //    a un calculo no rastreable -> escapa.
                if (is_derivation(ins.op)) {
                    if (ins.dst != ir::IR_NO_VALUE &&
                        marks.same_root(o, ins.dst))
                        continue;
                    marks.escape(o);
                    continue;
                }
                // 4) CALL estatico: el arg escapa SOLO si el callee captura ese
                //    parametro (interproc).  El indice de param = posicion del
                //    operando (los args de CALL son operands[0..N-1]).
                if (is_static_call(ins.op)) {
                    if (callee(ins.func_name, static_cast<int32_t>(p)))
                        marks.escape(o);
                    continue;
                }
                // 5) CALL dinamico/nativo: callee desconocido -> captura todos.
                if (is_dynamic_call(ins.op)) {
                    marks.escape(o);
                    continue;
                }
                // 6) Cualquier otra posicion (STORE valor, RET, THROW,
                // SETSTATIC
                //    valor, atomic store, MAKE_CLOSURE, aritmetica
                //    no-derivacion sobre el puntero, ...): el VALOR del puntero
                //    se usa/guarda
                //    -> CAPTURA -> escapa.  Es el caso CORRECTO, no un bail.
                marks.escape(o);
            }
            // Valores retornados/lanzados que no van por operands (RET/THROW ya
            // usan operands, cubiertos arriba).
        }
    }
    return out;
}

namespace {

/**
 * @brief El oraculo interprocedural: si el callee @c name captura su
 *        parametro, segun lo que el modulo ya sabe de el.
 *
 * Un callee CONOCIDO escapa segun su resumen; uno DESCONOCIDO -- externo, sin
 * analizar -- captura todo, que es la respuesta correcta sin su cuerpo.
 */
struct ModuleEscapeOracle {
    /// Nombre -> nodo del grafo, solo de las funciones con cuerpo.
    const std::unordered_map<std::string_view, util::SccNode> *node_of;
    /// Nodo -> posicion de su funcion en el modulo.
    const util::NamedVector<ir::IrFnPos, scratch::EscapeNodePos> *pos_of;
    const ModuleEscape *res;
    bool operator()(const std::string &name, int32_t pidx) const {
        const auto it = node_of->find(name);
        if (it == node_of->end()) return true; // externo/no analizado -> captura
        return res->by_fn[(*pos_of)[it->second]].escaping_params.count(pidx) >
               0;
    }
};

/**
 * @brief Recalcula el escape de @p fn y dice si su resumen crecio.
 *
 * Crece monotono -- solo se anaden parametros y reservas que escapan --, asi
 * que comparar tamanos basta.
 */
bool refresh_escape(
    const ir::IrFunction &fn,
    const std::function<const IrFacts &(const ir::IrFunction &)> &facts_of,
    const std::function<const PointsTo &(const ir::IrFunction &)> &pt_of,
    const ModuleEscapeOracle &oracle, EscapeInfo &cur) {
    EscapeInfo e = compute_escape(fn, facts_of(fn), pt_of(fn), oracle);
    if (e.escaping_params.size() == cur.escaping_params.size() &&
        e.escaping_stack.size() == cur.escaping_stack.size())
        return false;
    cur = std::move(e);
    return true;
}

} // namespace

ModuleEscape compute_escape_module(
    const ir::IrModule &mod,
    const std::function<const IrFacts &(const ir::IrFunction &)> &facts_of,
    const std::function<const PointsTo &(const ir::IrFunction &)> &pt_of) {
    ModuleEscape res;
    /* Inicial: nada escapa; el punto fijo solo ANADE. */
    res.by_fn.resize(mod.functions.size());
    res.state.assign(mod.functions.size(), EscapeFnState::NO_BODY);

    /* Las funciones con cuerpo, y por nombre -- el que llevan las llamadas --.
     * Vistas sobre los nombres del modulo: nada se copia.  El nombre solo se
     * mira aqui y al resolver cada llamada una vez; el resto va por posicion. */
    std::vector<const ir::IrFunction *> fns;
    util::NamedVector<ir::IrFnPos, scratch::EscapeNodePos> pos_of;
    std::unordered_map<std::string_view, util::SccNode> node_of;
    for (size_t p = 0; p < mod.functions.size(); ++p) {
        const ir::IrFunction &fn = mod.functions[p];
        if (fn.is_native) continue;
        node_of.emplace(fn.name, static_cast<util::SccNode>(fns.size()));
        fns.push_back(&fn);
        pos_of.push_back(static_cast<ir::IrFnPos>(p));
        res.state[p] = EscapeFnState::ANALYZED;
    }
    const ModuleEscapeOracle oracle{&node_of, &pos_of, &res};
    const size_t n = fns.size();

    /* El grafo de llamadas estaticas, en plano.
     *
     * Antes el punto fijo recorria el MODULO ENTERO por vuelta hasta que nada
     * cambiaba, con un tope de dos vueltas por funcion: cada resumen nuevo
     * tardaba una vuelta en llegar a su llamante, asi que las vueltas crecian
     * con la profundidad de las llamadas -- vueltas por tamano del modulo --.
     * Por componentes fuertemente conexas, de los callees a los llamantes,
     * cada funcion fuera de un ciclo se calcula UNA vez con sus callees ya
     * cerrados, y solo un ciclo de llamadas se itera, y solo sobre si mismo.
     * El punto fijo al que se llega es el mismo: el menor. */
    util::NamedVector<util::SccEdge, scratch::EscapeCallOffsets> call_off(
        n + 1, util::SccEdge(0));
    util::NamedVector<util::SccNode, scratch::EscapeCallTargets> call_to;
    for (size_t i = 0; i < n; ++i) {
        for (const ir::IrBlock &b : fns[i]->blocks)
            for (const ir::IrInstr &in : b.instrs) {
                if (!is_static_call(in.op)) continue;
                const auto it = node_of.find(in.func_name);
                if (it != node_of.end()) call_to.push_back(it->second);
            }
        call_off[i + 1] = static_cast<util::SccEdge>(call_to.size());
    }
    util::NamedVector<util::SccComp, scratch::EscapeComp> comp(n);
    const size_t n_comps =
        util::tarjan_scc(call_off.data(), call_to.data(), n, comp.data());

    /* Los miembros de cada componente, contiguos.  Tarjan las numera en orden
     * topologico INVERSO: recorrerlas por numero es ir de callees a
     * llamantes. */
    util::NamedVector<util::SccEdge, scratch::EscapeCompOffsets> comp_off(
        n_comps + 1, util::SccEdge(0));
    for (size_t i = 0; i < n; ++i)
        comp_off[comp[i] + 1] = util::SccEdge(comp_off[comp[i] + 1] + 1);
    for (size_t c = 0; c < n_comps; ++c)
        comp_off[c + 1] = util::SccEdge(comp_off[c + 1] + comp_off[c]);
    util::NamedVector<util::SccNode, scratch::EscapeCompMembers> members(n);
    {
        util::NamedVector<util::SccEdge, scratch::EscapeCompOffsets> next(
            comp_off.begin(), comp_off.end() - 1);
        for (size_t i = 0; i < n; ++i) {
            members[next[comp[i]]] = static_cast<util::SccNode>(i);
            next[comp[i]] = util::SccEdge(next[comp[i]] + 1);
        }
    }

    for (size_t c = 0; c < n_comps; ++c) {
        const size_t lo = comp_off[c], hi = comp_off[c + 1];
        /* Fuera de un ciclo -- un solo miembro que no se llama a si mismo --
         * basta con una vez: sus callees ya estan cerrados. */
        bool recursive = hi - lo > 1;
        if (!recursive) {
            const util::SccNode only = members[lo];
            for (util::SccEdge e = call_off[only]; e < call_off[only + 1];
                 e = util::SccEdge(e + 1))
                if (call_to[e] == only) recursive = true;
        }
        bool changed = true;
        int guard = 0;
        const int cap = static_cast<int>(hi - lo) * 2 + 8;
        while (changed && guard++ < cap) {
            changed = false;
            for (size_t k = lo; k < hi; ++k) {
                const util::SccNode node = members[k];
                if (refresh_escape(*fns[node], facts_of, pt_of, oracle,
                                   res.by_fn[pos_of[node]]))
                    changed = true;
            }
            if (!recursive) break;
        }
    }
    return res;
}

} // namespace analysis
