/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file fields_exprs.h
 * @brief Los campos de las EXPRESIONES (y de `MatchArm`).
 *
 * Solo lo incluye `vx/ast/fields.h`.  Un campo nuevo en uno de estos nodos va
 * tambien a su lista, con su papel: lo escribe el parser o lo anota despues el
 * comprobador o la bajada.
 */

#ifndef VX_AST_FIELDS_EXPRS_H
#define VX_AST_FIELDS_EXPRS_H

namespace vx::ast::fields {

/// Lo comun a toda expresion: todo lo anota el comprobador.
template <> struct Of<Expr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(
                annotated(&Expr::result_type, "result_type"),
                annotated(&Expr::borrow_owner_source, "borrow_owner_source"),
                annotated(&Expr::borrow_reborrow_source_name,
                          "borrow_reborrow_source_name"),
                annotated(&Expr::borrow_reborrow_source_is_mut,
                          "borrow_reborrow_source_is_mut"),
                annotated(&Expr::comptime_const_resolved,
                          "comptime_const_resolved"),
                annotated(&Expr::comptime_const_is_str, "comptime_const_is_str"),
                annotated(&Expr::comptime_const, "comptime_const")));
    }
};

template <> struct Of<IntLitExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(parsed(&IntLitExpr::value, "value"),
                            parsed(&IntLitExpr::suffix, "suffix"),
                            parsed(&IntLitExpr::negated, "negated")));
    }
};

template <> struct Of<FloatLitExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(parsed(&FloatLitExpr::value, "value"),
                            parsed(&FloatLitExpr::suffix, "suffix")));
    }
};

template <> struct Of<BoolLitExpr> {
    static constexpr auto list() {
        return std::tuple_cat(Of<Expr>::list(),
                              std::make_tuple(
                                  parsed(&BoolLitExpr::value, "value")));
    }
};

template <> struct Of<NullLitExpr> {
    static constexpr auto list() { return Of<Expr>::list(); }
};

template <> struct Of<CharLitExpr> {
    static constexpr auto list() {
        return std::tuple_cat(Of<Expr>::list(),
                              std::make_tuple(parsed(&CharLitExpr::codepoint,
                                                     "codepoint")));
    }
};

template <> struct Of<StringLitExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(
                parsed(&StringLitExpr::value, "value"),
                parsed(&StringLitExpr::is_raw, "is_raw"),
                parsed(&StringLitExpr::is_variadic, "is_variadic"),
                parsed(&StringLitExpr::interp_parts, "interp_parts"),
                parsed(&StringLitExpr::interp_exprs, "interp_exprs"),
                parsed(&StringLitExpr::interp_formats, "interp_formats")));
    }
};

template <> struct Of<IdentExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(
                parsed(&IdentExpr::name, "name"),
                annotated(&IdentExpr::is_func_ref, "is_func_ref"),
                annotated(&IdentExpr::func_ref_mangled, "func_ref_mangled")));
    }
};

template <> struct Of<ThisExpr> {
    static constexpr auto list() { return Of<Expr>::list(); }
};

template <> struct Of<FieldAccessExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(
                parsed(&FieldAccessExpr::base, "base"),
                parsed(&FieldAccessExpr::field_name, "field_name"),
                parsed(&FieldAccessExpr::ns_qualifier, "ns_qualifier"),
                annotated(&FieldAccessExpr::property_kind, "property_kind"),
                annotated(&FieldAccessExpr::ns_index, "ns_index"),
                annotated(&FieldAccessExpr::ns_sym, "ns_sym"),
                annotated(&FieldAccessExpr::is_func_ref, "is_func_ref"),
                annotated(&FieldAccessExpr::func_ref_mangled,
                          "func_ref_mangled"),
                annotated(&FieldAccessExpr::resolved_method,
                          "resolved_method")));
    }
};

template <> struct Of<BinaryExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(
                parsed(&BinaryExpr::op, "op"), parsed(&BinaryExpr::lhs, "lhs"),
                parsed(&BinaryExpr::rhs, "rhs"),
                // Lo pone la bajada al ver un cast al mismo tipo.
                annotated(&BinaryExpr::wrap_declared, "wrap_declared"),
                annotated(&BinaryExpr::overload_method, "overload_method"),
                annotated(&BinaryExpr::overload_negate, "overload_negate")));
    }
};

template <> struct Of<UnaryExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(
                parsed(&UnaryExpr::op, "op"),
                parsed(&UnaryExpr::operand, "operand"),
                annotated(&UnaryExpr::overload_method, "overload_method"),
                annotated(&UnaryExpr::desugared_bound_method,
                          "desugared_bound_method"),
                annotated(&UnaryExpr::bound_recv_name, "bound_recv_name"),
                annotated(&UnaryExpr::bound_recv_init, "bound_recv_init")));
    }
};

template <> struct Of<AssignExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(
                parsed(&AssignExpr::op, "op"),
                parsed(&AssignExpr::target, "target"),
                parsed(&AssignExpr::value, "value"),
                annotated(&AssignExpr::overload_method, "overload_method")));
    }
};

template <> struct Of<TernaryExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(parsed(&TernaryExpr::cond, "cond"),
                            parsed(&TernaryExpr::then_expr, "then_expr"),
                            parsed(&TernaryExpr::else_expr, "else_expr")));
    }
};

template <> struct Of<TryExpr> {
    static constexpr auto list() {
        return std::tuple_cat(Of<Expr>::list(),
                              std::make_tuple(
                                  parsed(&TryExpr::operand, "operand")));
    }
};

template <> struct Of<CallExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(
                parsed(&CallExpr::callee, "callee"),
                parsed(&CallExpr::args, "args"),
                parsed(&CallExpr::arg_names, "arg_names"),
                parsed(&CallExpr::has_receiver_hole, "has_receiver_hole"),
                parsed(&CallExpr::type_args, "type_args"),
                parsed(&CallExpr::is_braces_call, "is_braces_call"),
                annotated(&CallExpr::comptime_ctor_type, "comptime_ctor_type"),
                annotated(&CallExpr::macro_expanded, "macro_expanded"),
                annotated(&CallExpr::is_indirect_call, "is_indirect_call"),
                annotated(&CallExpr::is_default_struct_ctor,
                          "is_default_struct_ctor"),
                annotated(&CallExpr::resolved_sig, "resolved_sig"),
                annotated(&CallExpr::resolved_method, "resolved_method")));
    }
};

template <> struct Of<IndexExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(
                parsed(&IndexExpr::base, "base"),
                parsed(&IndexExpr::index, "index"),
                parsed(&IndexExpr::is_range, "is_range"),
                parsed(&IndexExpr::range_inclusive, "range_inclusive"),
                parsed(&IndexExpr::range_hi, "range_hi"),
                annotated(&IndexExpr::overload_method, "overload_method"),
                annotated(&IndexExpr::index_set_method, "index_set_method"),
                annotated(&IndexExpr::is_overlay_array, "is_overlay_array")));
    }
};

template <> struct Of<NewExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(parsed(&NewExpr::class_name, "class_name"),
                            parsed(&NewExpr::args, "args"),
                            parsed(&NewExpr::type_args, "type_args"),
                            parsed(&NewExpr::array_size, "array_size"),
                            annotated(&NewExpr::resolved_method,
                                      "resolved_method"),
                            annotated(&NewExpr::is_shared, "is_shared"),
                            annotated(&NewExpr::is_gc, "is_gc"),
                            annotated(&NewExpr::is_mangled, "is_mangled")));
    }
};

template <> struct Of<SpawnExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(parsed(&SpawnExpr::policy, "policy"),
                            parsed(&SpawnExpr::sched_idx, "sched_idx"),
                            parsed(&SpawnExpr::body, "body")));
    }
};

template <> struct Of<RSpawnExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(parsed(&RSpawnExpr::node_idx, "node_idx"),
                            parsed(&RSpawnExpr::body, "body")));
    }
};

template <> struct Of<LambdaExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(
                parsed(&LambdaExpr::params, "params"),
                parsed(&LambdaExpr::return_type, "return_type"),
                parsed(&LambdaExpr::body, "body"),
                annotated(&LambdaExpr::captures, "captures"),
                annotated(&LambdaExpr::capture_types, "capture_types"),
                annotated(&LambdaExpr::mutable_captures, "mutable_captures"),
                annotated(&LambdaExpr::synthetic_name, "synthetic_name"),
                annotated(&LambdaExpr::env_in_heap, "env_in_heap"),
                annotated(&LambdaExpr::env_owned_by_field,
                          "env_owned_by_field")));
    }
};

/// Una rama de un `match` (no es un nodo, pero tiene hijos).  La guarda va
/// ANTES que el cuerpo porque es el orden en que se evaluan: el recorrido de
/// hijos (`vx/ast/children.h`) sigue esta lista.
template <> struct Of<MatchArm> {
    static constexpr auto list() {
        return std::make_tuple(
            parsed(&MatchArm::variant_name, "variant_name"),
            parsed(&MatchArm::bindings, "bindings"),
            parsed(&MatchArm::value_pattern, "value_pattern"),
            parsed(&MatchArm::value_pattern_hi, "value_pattern_hi"),
            parsed(&MatchArm::range_inclusive, "range_inclusive"),
            parsed(&MatchArm::guard, "guard"), parsed(&MatchArm::body, "body"),
            parsed(&MatchArm::loc, "loc"));
    }
};

template <> struct Of<MatchExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(parsed(&MatchExpr::scrutinee, "scrutinee"),
                            parsed(&MatchExpr::arms, "arms")));
    }
};

template <> struct Of<SuperCallExpr> {
    static constexpr auto list() {
        return std::tuple_cat(Of<Expr>::list(),
                              std::make_tuple(
                                  parsed(&SuperCallExpr::args, "args")));
    }
};

template <> struct Of<SuperMethodCallExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(
                parsed(&SuperMethodCallExpr::method_name, "method_name"),
                parsed(&SuperMethodCallExpr::args, "args"),
                annotated(&SuperMethodCallExpr::resolved_owner,
                          "resolved_owner"),
                annotated(&SuperMethodCallExpr::resolved_method,
                          "resolved_method")));
    }
};

template <> struct Of<InitListExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(
                parsed(&InitListExpr::elements, "elements"),
                parsed(&InitListExpr::field_names, "field_names"),
                parsed(&InitListExpr::is_designated, "is_designated"),
                annotated(&InitListExpr::target_type, "target_type")));
    }
};

template <> struct Of<CastExpr> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Expr>::list(),
            std::make_tuple(parsed(&CastExpr::target_type, "target_type"),
                            parsed(&CastExpr::operand, "operand")));
    }
};

} // namespace vx::ast::fields

#endif // VX_AST_FIELDS_EXPRS_H
