/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/subscript_call.cpp
 * @brief Bajar un operador de subindice de un struct o clase: `x[i]`,
 *        `x[i] = v` y `x[a..b]` son llamadas a `__index__`, `__index_set__` y
 *        `__slice__` / `__slice_mut__`.
 *
 * Un camino para los cuatro.  La lectura y la escritura robaban cada una los
 * hijos del nodo para montar su llamada, por separado; el corte ademas necesita
 * pasar valores YA bajados -- los limites normalizados, la longitud -- y
 * evaluar el receptor una sola vez.  Por eso el receptor y los limites se ligan
 * a nombres internos en un ambito propio, y la llamada que se monta los nombra:
 * baja por el camino normal de los metodos (struct, clase, virtual, extension,
 * buffer de retorno) sin repetir nada de el.
 */

#include "ir/synthetic_symbols.h"
#include "vx/lowering.h"
#include "vx/method_names.h"

#include <memory>
#include <utility>

namespace vx {

/**
 * @brief Un nombre interno ya ligado, como expresion para la llamada montada.
 * @param name El nombre (de `synthetic_symbols.h`).
 * @param t    El tipo con el que se lee.
 * @param loc  Donde se escribio el subindice.
 * @return El nodo.
 */
static std::unique_ptr<ast::Expr> bound_name(const char *name, const Type &t,
                                             const SourceLoc &loc) {
    auto id = std::make_unique<ast::IdentExpr>();
    id->loc = loc;
    id->name = name;
    id->result_type = t;
    return id;
}

ir::IrValueId Lowering::call_on_subscript_receiver_(
    const SourceLoc &loc, const Type &recv_t, const char *method,
    std::vector<std::unique_ptr<ast::Expr>> &args) {
    ast::CallExpr call;
    call.loc = loc;
    auto fa = std::make_unique<ast::FieldAccessExpr>();
    fa->loc = loc;
    fa->field_name = method;
    fa->base = bound_name(ir::kSubscriptRecv, recv_t, loc);
    call.callee = std::move(fa);
    call.args = std::move(args);
    const ir::IrValueId v = recv_t.kind == PrimitiveKind::STRUCT
                                ? lower_struct_method_call(&call)
                                : lower_class_method_call(&call);
    args = std::move(call.args); // quien los presto los recupera
    return v;
}

ir::IrValueId
Lowering::lower_subscript_operator(ast::IndexExpr *e,
                                   std::unique_ptr<ast::Expr> *written_value) {
    const std::string &method =
        written_value != nullptr ? e->index_set_method : e->overload_method;
    const Type recv_t = e->base->result_type;

    push_scope();
    // El receptor se evalua UNA vez: todo lo demas lo nombra.
    const ir::IrValueId v_recv = lower_expr(e->base.get());
    if (v_recv == ir::IR_NO_VALUE) {
        pop_scope();
        return ir::IR_NO_VALUE;
    }
    bind(ir::kSubscriptRecv, v_recv);

    std::vector<std::unique_ptr<ast::Expr>> args;
    ir::IrValueId out = ir::IR_NO_VALUE;
    const bool slices = method == kSliceMethod || method == kSliceMutMethod;
    if (slices) {
        /* Los limites, normalizados a `[lo, hi)` por su dueno.  Sin superior,
         * hasta `len()` del propio receptor (el comprobador ya exigio que lo
         * declare). */
        ir::IrValueId v_len = ir::IR_NO_VALUE;
        if (!e->range_hi) {
            std::vector<std::unique_ptr<ast::Expr>> no_args;
            v_len = call_on_subscript_receiver_(e->loc, recv_t, kLengthMethod,
                                                no_args);
            if (v_len != ir::IR_NO_VALUE)
                v_len = cast_if_needed(v_len, fn_->values[v_len].type,
                                       ir::IrType::I64, e->loc.line);
        }
        ir::IrValueId v_lo = ir::IR_NO_VALUE;
        ir::IrValueId v_hi = ir::IR_NO_VALUE;
        if (lower_range_bounds(e, v_len, v_lo, v_hi)) {
            bind(ir::kSubscriptLo, v_lo);
            bind(ir::kSubscriptHi, v_hi);
            const Type t_bound{PrimitiveKind::I64};
            args.push_back(bound_name(ir::kSubscriptLo, t_bound, e->loc));
            args.push_back(bound_name(ir::kSubscriptHi, t_bound, e->loc));
            out = call_on_subscript_receiver_(e->loc, recv_t, method.c_str(),
                                              args);
        }
    } else {
        /* `__index__(i)` y `__index_set__(i, v)`: los argumentos son lo que se
         * escribio, prestado al nodo montado y devuelto despues. */
        args.push_back(std::move(e->index));
        if (written_value != nullptr) args.push_back(std::move(*written_value));
        out = call_on_subscript_receiver_(e->loc, recv_t, method.c_str(), args);
        e->index = std::move(args[0]);
        if (written_value != nullptr) *written_value = std::move(args[1]);
    }
    pop_scope();
    return out;
}

} // namespace vx
