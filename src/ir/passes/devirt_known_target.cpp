/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file devirt_known_target.cpp
 * @brief Implementacion de @ref ir::ir_pass_devirt_known_target.
 *
 * Tres piezas, en este orden: saber a que funcion apunta cada valor (por SSA,
 * por la direccion nativa y por memoria), decidir la forma directa de cada
 * llamada segun lo que su destino espera del entorno, y reescribir.
 */

#include "ir/passes/devirt_known_target.h"

#include "analysis/asa/fact_base.h"
#include "analysis/memory/points_to.h"
#include "ir/ir_optimizer.h"
#include "ir/passes/block_order.h"
#include "ir/passes/guarded_call.h"
#include "util/env_flags.h"

#include <utility>
#include <vector>

namespace ir {

namespace scratch {
struct KnownTargetOfValue; ///< Valor -> la funcion a la que apunta.
struct KnownConstOfValue;  ///< Valor -> la constante que lleva.
struct KnownIsConstDef;    ///< Valor -> si lo define un CONST.
struct KnownLoadAddrOf;    ///< Valor cargado -> la direccion de la que sale.
struct KnownPendingCalls;  ///< Llamadas cuyo destino sale de memoria.
} // namespace scratch

LabelAddrIndex::LabelAddrIndex(const IrFunction &fn) {
    by_value_.resize(fn.values.size());
    for (const IrBlock &b : fn.blocks)
        for (const IrInstr &in : b.instrs)
            if (in.op == IrOp::LABEL_ADDR && in.dst < by_value_.size() &&
                !in.func_name.empty())
                by_value_[in.dst] = util::InternedName::intern(in.func_name);
}

util::InternedName known_closure_target(const IrInstr &call,
                                        const LabelAddrIndex &labels) {
    if (call.op != IrOp::CALLCLOSURE || call.func_ptr == IR_NO_VALUE ||
        call.operands.empty())
        return util::InternedName();
    return labels.label_of(call.func_ptr);
}

CalleeShapes ir_callee_shapes(const IrModule &mod) {
    CalleeShapes shapes;
    shapes.reserve(mod.functions.size());
    for (const IrFunction &f : mod.functions) {
        CalleeShape s;
        s.params = f.params.size();
        for (const IrBlock &b : f.blocks)
            for (const IrInstr &in : b.instrs)
                s.vm_reg_reads =
                    join_vm_reg_reads(s.vm_reg_reads, vm_reg_reads_of(in));
        shapes[util::InternedName::from_interned(f.name_key())] = s;
    }
    return shapes;
}

namespace {

/**
 * @brief A que funcion apunta cada valor de una funcion, por todos los caminos
 *        que la hacen conocida.
 */
struct KnownTargets {
    /// Por valor, la funcion a la que apunta (vacio si no se sabe).
    util::NamedVector<util::InternedName, scratch::KnownTargetOfValue> target;
};

/**
 * @brief Si @p op es una llamada cuyo destino viaja en @c func_ptr.
 * @param op Operacion.
 * @return true para @c CALLIND y @c CALLCLOSURE.
 */
bool is_pointer_call(IrOp op) {
    return op == IrOp::CALLIND || op == IrOp::CALLCLOSURE;
}

/**
 * @brief Resuelve por SSA: @c LABEL_ADDR, la direccion nativa y los @c MOV.
 *
 * En orden lineal, como hacia el pase de `cfn`: un @c MOV hereda el destino de
 * lo que copia si ya se vio.
 *
 * @param fn    Funcion.
 * @param index Las funciones por su hash.
 * @param out   Recibe el destino de cada valor.
 * @param load_addr_of Recibe, por valor cargado, la direccion de la carga.
 */
void resolve_through_ssa(
    const IrFunction &fn, const NakedFnAddrIndex &index, KnownTargets &out,
    util::NamedVector<IrValueId, scratch::KnownLoadAddrOf> &load_addr_of) {
    const size_t nv = fn.values.size();
    out.target.assign(nv, util::InternedName());
    load_addr_of.assign(nv, IR_NO_VALUE);
    util::NamedVector<uint64_t, scratch::KnownConstOfValue> const_of(nv, 0);
    util::NamedVector<char, scratch::KnownIsConstDef> is_const(nv, 0);
    for (const IrBlock &b : fn.blocks) {
        for (const IrInstr &in : b.instrs) {
            if (in.dst == IR_NO_VALUE || in.dst >= nv) continue;
            switch (in.op) {
            case IrOp::CONST:
                const_of[in.dst] = static_cast<uint64_t>(in.imm);
                is_const[in.dst] = 1;
                break;
            case IrOp::LABEL_ADDR:
                if (!in.func_name.empty())
                    out.target[in.dst] =
                        util::InternedName::intern(in.func_name);
                break;
            case IrOp::CALLN:
                /* La direccion NATIVA de una funcion plana: el hash es una
                 * constante de compilacion y se deshace con el indice.  Una
                 * @Naked no esta en el a proposito (ver NakedFnAddrIndex). */
                if (!index.empty() && in.func_name == "vrt:naked_fnaddr" &&
                    in.operands.size() >= 2 && in.operands[1] < nv &&
                    is_const[in.operands[1]]) {
                    if (const std::string *nm =
                            index.find(const_of[in.operands[1]]))
                        out.target[in.dst] = util::InternedName::intern(*nm);
                }
                break;
            case IrOp::MOV:
                if (!in.operands.empty() && in.operands[0] < nv)
                    out.target[in.dst] = out.target[in.operands[0]];
                break;
            case IrOp::LOAD:
                if (!in.operands.empty()) load_addr_of[in.dst] = in.operands[0];
                break;
            default: break;
            }
        }
    }
}

/**
 * @brief Resuelve por MEMORIA los destinos que el camino SSA no alcanzo.
 *
 * Un destino guardado una sola vez y leido de vuelta -- un campo, un
 * `unique<cfn>`, la ranura de funcion de una closure -- es conocido, pero el
 * valor que llega a la carga no es el mismo SSA que el del almacen.  Quien
 * empareja los dos es points-to.  Solo se le pregunta por las llamadas que
 * quedaron sin resolver y cuyo destino sale de una carga; sin ninguna, no se
 * pide la tabla.
 *
 * @param fn   Funcion.
 * @param base A quien preguntarle.
 * @param load_addr_of De que direccion sale cada valor cargado.
 * @param out  Destinos; se completan los de las llamadas resueltas.
 */
void resolve_through_memory(
    const IrFunction &fn, analysis::asa::FactBase &base,
    const util::NamedVector<IrValueId, scratch::KnownLoadAddrOf> &load_addr_of,
    KnownTargets &out) {
    if (util::flag_on(util::FlagId::NoDevirtThroughMemory)) return;
    IrOperands pending_addrs;
    util::NamedVector<IrValueId, scratch::KnownPendingCalls> pending_ptrs;
    for (const IrBlock &b : fn.blocks) {
        for (const IrInstr &in : b.instrs) {
            if (!is_pointer_call(in.op) || in.func_ptr == IR_NO_VALUE ||
                in.func_ptr >= out.target.size())
                continue;
            if (!out.target[in.func_ptr].empty()) continue;
            const IrValueId addr = load_addr_of[in.func_ptr];
            if (addr == IR_NO_VALUE) continue;
            pending_addrs.push_back(addr);
            pending_ptrs.push_back(in.func_ptr);
        }
    }
    if (pending_addrs.empty()) return;
    const analysis::PointsTo &pt = base.memory(fn);
    const std::vector<IrValueId> stored = analysis::single_values_at(
        fn, pt, pending_addrs, static_cast<int32_t>(sizeof(uint64_t)));
    for (size_t i = 0; i < pending_ptrs.size(); ++i) {
        if (stored[i] == IR_NO_VALUE || stored[i] >= out.target.size())
            continue;
        out.target[pending_ptrs[i]] = out.target[stored[i]];
    }
}

/**
 * @brief Si @p v lo define un @c LABEL_ADDR de @p fn: la llamada ya escribe su
 *        destino y no hay que tocarla.
 * @param labels Los @c LABEL_ADDR de la funcion.
 * @param v      Valor.
 * @return true si lo es.
 */
bool is_written_target(const LabelAddrIndex &labels, IrValueId v) {
    return !labels.label_of(v).empty();
}

/**
 * @brief Reescribe una @c CALLCLOSURE de destino @p target a su forma directa.
 *
 * @param fn     Funcion que la contiene.
 * @param instrs Instrucciones del bloque; puede recibir un @c LABEL_ADDR.
 * @param pos    Posicion de la llamada; avanza si se inserta delante.
 * @param target Destino conocido.
 * @param shape  Lo que ese destino espera.
 * @param labels Los @c LABEL_ADDR que ya tenia la funcion.
 * @return true si la cambio.
 */
bool direct_closure_call(IrFunction &fn, std::vector<IrInstr> &instrs,
                         size_t &pos, util::InternedName target,
                         const CalleeShape &shape,
                         const LabelAddrIndex &labels) {
    IrInstr &cc = instrs[pos];
    const size_t nargs = cc.operands.size() - 1;
    const IrValueId env = cc.operands[0];
    /* Un registro que ninguna llamada fija no tiene con que sustituirse. */
    if (shape.vm_reg_reads == VmRegReads::Other) return false;
    const bool env_as_param = shape.params == nargs + 1;
    const bool env_unused =
        shape.params == nargs && shape.vm_reg_reads == VmRegReads::None;
    if (env_as_param || env_unused) {
        /* El entorno ya viaja como parametro (convencion nativa) o no se usa:
         * es un CALL corriente. */
        IrOperands args;
        for (size_t k = 1; k < cc.operands.size(); ++k)
            args.push_back(cc.operands[k]);
        if (env_as_param) args.push_back(env);
        cc.op = IrOp::CALL;
        cc.func_name = target.str();
        cc.func_ptr = IR_NO_VALUE;
        cc.operands = std::move(args);
        return true;
    }
    if (shape.params != nargs) return false; // aridad que no encaja
    /* Lee el entorno del registro: sigue siendo CALLCLOSURE, pero con el
     * destino ESCRITO -- una llamada directa que entrega el entorno --. */
    if (is_written_target(labels, cc.func_ptr)) return false;
    IrInstr la;
    la.op = IrOp::LABEL_ADDR;
    la.type = IrType::PTR;
    la.dst = fn.new_value(IrType::PTR);
    la.func_name = target.str();
    la.source_line = cc.source_line;
    la.inline_site = cc.inline_site;
    const IrValueId label = la.dst;
    instrs.insert(instrs.begin() + static_cast<std::ptrdiff_t>(pos),
                  std::move(la));
    ++pos;
    instrs[pos].func_ptr = label;
    return true;
}

/**
 * @brief Los operandos que tendria el @c CALL directo de un despacho: sin lo
 *        que solo servia para despachar.
 *
 * Receptor, retorno por buffer si lo hay, argumentos.  Se sueltan los
 * parametros de busqueda de un @c CALLITF y la clase base de un
 * @c CALLSUPER; el puntero de un @c CALLIND no es operando.
 *
 * @param in  El despacho.
 * @param out Recibe los operandos.
 * @return false si la forma no es un despacho con destino demostrable.
 */
bool direct_operands(const IrInstr &in, IrOperands &out) {
    out = in.operands;
    switch (in.op) {
    case IrOp::CALLVIRT:
    case IrOp::CALLIND: return !out.empty();
    case IrOp::CALLITF: // [receptor, params, args...]
    case IrOp::CALLM:   // [receptor, MethodInfo*, args...]
        if (out.size() < 2) return false;
        out.erase_at(1);
        return true;
    case IrOp::CALLSUPER: // [clase base, this, args...]
        if (out.size() < 2) return false;
        out.erase_at(0);
        return true;
    default: return false;
    }
}

/**
 * @brief Hace directo un despacho dinamico cuyo destino DEMUESTRA la
 *        jerarquia (@c IrInstr::proven_callee).
 *
 * El despacho hacia algo de paso: leer la clase del receptor, que sobre nulo
 * LANZA -- y capturable, a diferencia de un `unwrap` --.  Una llamada directa
 * no lee nada, asi que sin guarda `nul.get()` dejaba de lanzar.  La forma es
 * la de toda llamada directa condicionada (guarded_call.h): si el receptor no
 * es nulo, directa; si lo es, el despacho ORIGINAL, que lanza lo mismo en el
 * mismo punto.  Donde se demuestra que no es nulo (`this`, un `new`), la
 * guarda se pliega sola en los pases de nulos.  `super` no la necesita: su
 * receptor es `this`, y se reescribe en el sitio.
 *
 * @param fn    Funcion que contiene la llamada.
 * @param block Bloque de la llamada.
 * @param pos   Posicion de la llamada.
 * @return true si la reescribio.
 */
bool direct_proven_dispatch(IrFunction &fn, IrBlockId block, size_t pos) {
    IrInstr &in = fn.blocks[block].instrs[pos];
    if (in.proven_callee.empty()) return false;
    IrOperands fast;
    if (!direct_operands(in, fast)) return false;
    const util::InternedName callee = in.proven_callee;
    in.proven_callee = util::InternedName(); // el respaldo no se reprocesa
    if (in.op == IrOp::CALLSUPER) {
        in.op = IrOp::CALL;
        in.func_name = callee.str();
        in.imm = 0;
        in.operands = fast;
        return true;
    }
    /* guarda = (receptor != 0), delante de la llamada. */
    const IrValueId recv = fast[0];
    const IrType rt = recv < fn.values.size() ? fn.values[recv].type
                                              : IrType::PTR;
    const uint32_t line = in.source_line;
    const IrValueId zero = fn.new_value(rt);
    fn.values[zero].is_const = true;
    fn.values[zero].const_val = 0;
    const IrValueId cond = fn.new_value(IrType::BOOL);
    IrInstr kz;
    kz.op = IrOp::CONST;
    kz.type = rt;
    kz.dst = zero;
    kz.imm = 0;
    kz.source_line = line;
    IrInstr cmp;
    cmp.op = IrOp::CMP_NE;
    cmp.type = IrType::BOOL;
    cmp.dst = cond;
    cmp.operands = {recv, zero};
    cmp.source_line = line;
    std::vector<IrInstr> &b = fn.blocks[block].instrs;
    b.insert(b.begin() + static_cast<std::ptrdiff_t>(pos),
             {std::move(kz), std::move(cmp)});
    const GuardedArm arm{cond, callee};
    split_guarded_call(fn, block, pos + 2, fast, &arm, 1);
    return true;
}

/**
 * @brief Cuerpo del pase; la puerta publica lo envuelve.
 * @return true si reescribio alguna llamada.
 */
bool devirt_known_target_impl(IrFunction &fn, const NakedFnAddrIndex &index,
                              const CalleeShapes &shapes,
                              analysis::asa::FactBase &base) {
    if (fn.is_native || fn.blocks.empty()) return false;
    /* Primero lo DEMOSTRADO por la jerarquia: no hace falta buscar nada.  Por
     * indice: partir un bloque anade otros al final, y el recorrido los visita
     * (llevan el resto del partido); tras partir, el bloque acaba en la
     * guarda y no queda nada mas en el. */
    bool changed = false;
    bool split_any = false;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi)
        for (size_t i = 0; i < fn.blocks[bi].instrs.size(); ++i) {
            const IrOp before = fn.blocks[bi].instrs[i].op;
            if (!direct_proven_dispatch(fn, IrBlockId(bi), i)) continue;
            changed = true;
            if (before != IrOp::CALLSUPER) split_any = true;
        }
    if (split_any) reorder_blocks_rpo(fn);
    bool any_pointer_call = false;
    for (const IrBlock &b : fn.blocks)
        for (const IrInstr &in : b.instrs)
            if (is_pointer_call(in.op)) any_pointer_call = true;
    if (!any_pointer_call) return changed;

    KnownTargets known;
    util::NamedVector<IrValueId, scratch::KnownLoadAddrOf> load_addr_of;
    resolve_through_ssa(fn, index, known, load_addr_of);
    resolve_through_memory(fn, base, load_addr_of, known);
    const LabelAddrIndex labels(fn);

    for (IrBlock &b : fn.blocks) {
        for (size_t i = 0; i < b.instrs.size(); ++i) {
            IrInstr &in = b.instrs[i];
            if (!is_pointer_call(in.op) || in.func_ptr == IR_NO_VALUE ||
                in.func_ptr >= known.target.size())
                continue;
            const util::InternedName target = known.target[in.func_ptr];
            if (target.empty()) continue;
            if (in.op == IrOp::CALLIND) {
                in.op = IrOp::CALL;
                in.func_name = target.str();
                in.func_ptr = IR_NO_VALUE;
                changed = true;
                continue;
            }
            /* CALLCLOSURE: su forma directa depende de lo que el destino
             * espera del entorno.  Un destino fuera del modulo no se toca. */
            if (in.operands.empty()) continue;
            const auto sh = shapes.find(target);
            if (sh == shapes.end()) continue;
            if (direct_closure_call(fn, b.instrs, i, target, sh->second,
                                    labels))
                changed = true;
        }
    }
    return changed;
}

} // namespace

PassResult ir_pass_devirt_known_target(IrFunction &fn,
                                       const NakedFnAddrIndex &index,
                                       const CalleeShapes &shapes,
                                       analysis::asa::FactBase &base) {
    return PassResult::of(fn,
                          devirt_known_target_impl(fn, index, shapes, base));
}

bool ir_devirt_known_targets(IrModule &mod) {
    const NakedFnAddrIndex index = ir_naked_fnaddr_index(mod);
    const CalleeShapes shapes = ir_callee_shapes(mod);
    analysis::asa::FactBase base(analysis::asa::kStageDuringOpt);
    bool any = false;
    for (IrFunction &fn : mod.functions)
        if (applied(ir_pass_devirt_known_target(fn, index, shapes, base)))
            any = true;
    return any;
}

} // namespace ir
