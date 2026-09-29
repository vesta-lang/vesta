/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file fields_stmts.h
 * @brief Los campos de las SENTENCIAS (y de `CatchClause` y `AsmOperand`).
 *
 * Solo lo incluye `vx/ast/fields.h`.  Un campo nuevo en uno de estos nodos va
 * tambien a su lista, con su papel.
 */

#ifndef VX_AST_FIELDS_STMTS_H
#define VX_AST_FIELDS_STMTS_H

namespace vx::ast::fields {

template <> struct Of<Stmt> {
    static constexpr auto list() { return Of<Node>::list(); }
};

template <> struct Of<BlockStmt> {
    static constexpr auto list() {
        return std::tuple_cat(Of<Stmt>::list(),
                              std::make_tuple(parsed(&BlockStmt::body, "body")));
    }
};

template <> struct Of<VarDeclStmt> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Stmt>::list(),
            std::make_tuple(
                parsed(&VarDeclStmt::type, "type"),
                parsed(&VarDeclStmt::name, "name"),
                parsed(&VarDeclStmt::init, "init"),
                parsed(&VarDeclStmt::is_const, "is_const"),
                parsed(&VarDeclStmt::dir, "dir"),
                parsed(&VarDeclStmt::is_comptime, "is_comptime"),
                parsed(&VarDeclStmt::infer_type, "infer_type"),
                parsed(&VarDeclStmt::is_shared, "is_shared"),
                parsed(&VarDeclStmt::reg_binding, "reg_binding"),
                parsed(&VarDeclStmt::is_static, "is_static"),
                annotated(&VarDeclStmt::declared_deleter,
                          "declared_deleter")));
    }
};

template <> struct Of<ExprStmt> {
    static constexpr auto list() {
        return std::tuple_cat(Of<Stmt>::list(),
                              std::make_tuple(parsed(&ExprStmt::expr, "expr")));
    }
};

template <> struct Of<ComptimeBlockStmt> {
    static constexpr auto list() {
        return std::tuple_cat(Of<Stmt>::list(),
                              std::make_tuple(parsed(&ComptimeBlockStmt::stmts,
                                                     "stmts")));
    }
};

template <> struct Of<ComptimeForStmt> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Stmt>::list(),
            std::make_tuple(parsed(&ComptimeForStmt::var_name, "var_name"),
                            parsed(&ComptimeForStmt::lo_expr, "lo_expr"),
                            parsed(&ComptimeForStmt::hi_expr, "hi_expr"),
                            parsed(&ComptimeForStmt::inclusive, "inclusive"),
                            parsed(&ComptimeForStmt::body, "body")));
    }
};

template <> struct Of<IfStmt> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Stmt>::list(),
            std::make_tuple(parsed(&IfStmt::cond, "cond"),
                            parsed(&IfStmt::then_branch, "then_branch"),
                            parsed(&IfStmt::else_branch, "else_branch"),
                            parsed(&IfStmt::is_comptime, "is_comptime")));
    }
};

template <> struct Of<WhileStmt> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Stmt>::list(),
            std::make_tuple(parsed(&WhileStmt::cond, "cond"),
                            parsed(&WhileStmt::body, "body")));
    }
};

template <> struct Of<DoWhileStmt> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Stmt>::list(),
            std::make_tuple(parsed(&DoWhileStmt::body, "body"),
                            parsed(&DoWhileStmt::cond, "cond")));
    }
};

template <> struct Of<ForStmt> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Stmt>::list(),
            std::make_tuple(
                parsed(&ForStmt::init, "init"), parsed(&ForStmt::cond, "cond"),
                parsed(&ForStmt::step, "step"), parsed(&ForStmt::body, "body")));
    }
};

template <> struct Of<ReturnStmt> {
    static constexpr auto list() {
        return std::tuple_cat(Of<Stmt>::list(),
                              std::make_tuple(
                                  parsed(&ReturnStmt::value, "value")));
    }
};

template <> struct Of<ForEachStmt> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Stmt>::list(),
            std::make_tuple(parsed(&ForEachStmt::iter_type, "iter_type"),
                            parsed(&ForEachStmt::iter_name, "iter_name"),
                            parsed(&ForEachStmt::iter_expr, "iter_expr"),
                            parsed(&ForEachStmt::body, "body")));
    }
};

template <> struct Of<BreakStmt> {
    static constexpr auto list() { return Of<Stmt>::list(); }
};

template <> struct Of<ContinueStmt> {
    static constexpr auto list() { return Of<Stmt>::list(); }
};

template <> struct Of<GotoStmt> {
    static constexpr auto list() {
        return std::tuple_cat(Of<Stmt>::list(),
                              std::make_tuple(parsed(&GotoStmt::label, "label")));
    }
};

template <> struct Of<LabelStmt> {
    static constexpr auto list() {
        return std::tuple_cat(Of<Stmt>::list(),
                              std::make_tuple(parsed(&LabelStmt::name, "name")));
    }
};

/// Una clausula `catch` (no es un nodo, pero tiene hijos).
template <> struct Of<CatchClause> {
    static constexpr auto list() {
        return std::make_tuple(
            parsed(&CatchClause::loc, "loc"),
            parsed(&CatchClause::exc_class_name, "exc_class_name"),
            parsed(&CatchClause::var_name, "var_name"),
            parsed(&CatchClause::body, "body"));
    }
};

template <> struct Of<TryStmt> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Stmt>::list(),
            std::make_tuple(parsed(&TryStmt::body, "body"),
                            parsed(&TryStmt::catches, "catches"),
                            parsed(&TryStmt::finally_body, "finally_body")));
    }
};

template <> struct Of<ThrowStmt> {
    static constexpr auto list() {
        return std::tuple_cat(Of<Stmt>::list(),
                              std::make_tuple(
                                  parsed(&ThrowStmt::value, "value")));
    }
};

template <> struct Of<SynchronizedStmt> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Stmt>::list(),
            std::make_tuple(parsed(&SynchronizedStmt::target, "target"),
                            parsed(&SynchronizedStmt::body, "body")));
    }
};

/// Un enlace de la lista de operandos de un `asm`.
template <> struct Of<AsmOperand> {
    static constexpr auto list() {
        return std::make_tuple(parsed(&AsmOperand::reg_class, "reg_class"),
                               parsed(&AsmOperand::name, "name"),
                               parsed(&AsmOperand::init, "init"),
                               parsed(&AsmOperand::loc, "loc"));
    }
};

template <> struct Of<AsmStmt> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Stmt>::list(),
            std::make_tuple(
                parsed(&AsmStmt::body, "body"),
                parsed(&AsmStmt::level, "level"),
                parsed(&AsmStmt::operands, "operands"),
                parsed(&AsmStmt::body_loc, "body_loc"),
                parsed(&AsmStmt::q_volatile, "q_volatile"),
                parsed(&AsmStmt::q_nomem, "q_nomem"),
                parsed(&AsmStmt::q_preserves_flags, "q_preserves_flags"),
                parsed(&AsmStmt::q_pure, "q_pure"),
                parsed(&AsmStmt::q_noinfer, "q_noinfer"),
                parsed(&AsmStmt::clobbers, "clobbers"),
                parsed(&AsmStmt::clobbers_memory, "clobbers_memory"),
                parsed(&AsmStmt::clobbers_flags, "clobbers_flags")));
    }
};

} // namespace vx::ast::fields

#endif // VX_AST_FIELDS_STMTS_H
