/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file generic_clone.cpp
 * @brief Implementacion de las utilidades de clonacion de AST con
 *        sustitucion de type-params (monomorphizacion).
 *
 * Extraido de type_checker.cpp (que era un monolito) para mantener cada
 * fichero manejable.  Estas rutinas son AST-puras: clonan nodos AST
 * sustituyendo los type params por tipos concretos, sin tocar el estado
 * del type checker.  Las usan la monomorphizacion de clases, structs,
 * funciones libres y metodos genericos.
 */

#include "generic_clone.h"

namespace vex {
namespace vexgen {



std::string mangle_args(const std::vector<Type> &args) {
    std::string s;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) s += "_";
        s += mangle_type(args[i]);
    }
    return s;
}

std::string mangle_type(const Type &t) {
    switch (t.kind) {
    case PrimitiveKind::I8: return "i8";
    case PrimitiveKind::I16: return "i16";
    case PrimitiveKind::I32: return "i32";
    case PrimitiveKind::I64: return "i64";
    case PrimitiveKind::U8: return "u8";
    case PrimitiveKind::U16: return "u16";
    case PrimitiveKind::U32: return "u32";
    case PrimitiveKind::U64: return "u64";
    case PrimitiveKind::F32: return "f32";
    case PrimitiveKind::F64: return "f64";
    case PrimitiveKind::BOOL: return "bool";
    case PrimitiveKind::CHAR: return "ch";
    case PrimitiveKind::PTR: {
        // Incluir el pointee + la naturaleza (virtual vs host) para no colisionar
        // (`Caja<i64*>` vs `Caja<u8*>` vs `Caja<VirtualPtr<i64>>`).
        const std::string base = t.is_virtual ? "vptr" : "ptr";
        return t.pointee ? (base + mangle_type(*t.pointee)) : base;
    }
    case PrimitiveKind::ARRAY:
        return t.pointee ? ("arr" + mangle_type(*t.pointee)) : "arr";
    case PrimitiveKind::CLASS:
    case PrimitiveKind::STRUCT: return t.struct_name;
    case PrimitiveKind::STRING: return "str";
    default: return "x";
    }
}


// Reconstruye un TypeNode AST a partir de un Type ya resuelto.  Lo usa la
// sustitucion de type-params cuando el arg NO es un escalar simple: un puntero
// (`i64*`) o un array (`i64[4]`) deben preservar su pointee/element y tamano,
// no colapsar a un PrimitiveTypeNode{PTR/ARRAY} que pierde esa info (#2).
std::unique_ptr<ast::TypeNode> type_node_from_type(const Type &a,
                                                          const SourceLoc &loc) {
    switch (a.kind) {
    case PrimitiveKind::PTR: {
        auto p = std::make_unique<ast::PointerTypeNode>();
        p->loc = loc;
        // Preservar VirtualPtr<T> (is_virtual=true) vs T* (host).  Sin esto un
        // type-arg `VirtualPtr<i64>` colapsaba a `i64*` host -> el deref de un
        // `&local` (VM) por el campo fallaba.
        p->is_virtual = a.is_virtual;
        p->pointee = a.pointee ? type_node_from_type(*a.pointee, loc) : nullptr;
        return p;
    }
    case PrimitiveKind::ARRAY: {
        auto arr = std::make_unique<ast::ArrayTypeNode>();
        arr->loc = loc;
        arr->element_type =
            a.pointee ? type_node_from_type(*a.pointee, loc) : nullptr;
        if (a.array_size > 0) {
            auto sz = std::make_unique<ast::IntLitExpr>();
            sz->loc = loc;
            sz->value = static_cast<int64_t>(a.array_size);
            arr->size_expr = std::move(sz);
        }
        return arr;
    }
    case PrimitiveKind::CLASS:
    case PrimitiveKind::STRUCT: {
        auto n = std::make_unique<ast::NamedTypeNode>();
        n->loc = loc;
        n->name = a.struct_name;
        return n;
    }
    default: {
        auto p = std::make_unique<ast::PrimitiveTypeNode>();
        p->loc = loc;
        p->prim = a.kind;
        return p;
    }
    }
}

std::unique_ptr<ast::TypeNode>
clone_type_with_subst(const ast::TypeNode *t, const GenSubst &g) {
    if (!t) return nullptr;
    switch (t->kind) {
    case ast::NodeKind::PrimitiveTypeNode: {
        auto *src = static_cast<const ast::PrimitiveTypeNode *>(t);
        auto p = std::make_unique<ast::PrimitiveTypeNode>();
        p->loc = src->loc;
        p->prim = src->prim;
        // tipo args para colecciones genericas
        // (ArrayList<T>) deben replicarse al clonar el AST por
        // monomorphizacion u otras transformaciones.
        for (auto &ta : src->type_args) {
            p->type_args.push_back(clone_type_with_subst(ta.get(), g));
        }
        return p;
    }
    case ast::NodeKind::NamedTypeNode: {
        auto *src = static_cast<const ast::NamedTypeNode *>(t);
        // Si el nombre es uno de los type params, sustituimos por
        // el tipo concreto del binding.
        if (g.params && g.args) {
            for (size_t i = 0; i < g.params->size(); ++i) {
                if ((*g.params)[i] == src->name) {
                    // Reconstruir el TypeNode COMPLETO del arg (preserva
                    // puntero/array/pointee; antes un `i64*` colapsaba a un
                    // PrimitiveTypeNode{PTR} sin pointee -> deref fallaba, #2).
                    return type_node_from_type((*g.args)[i], src->loc);
                }
            }
        }
        auto n = std::make_unique<ast::NamedTypeNode>();
        n->loc = src->loc;
        n->name = src->name;
        for (auto &ta : src->type_args) {
            n->type_args.push_back(clone_type_with_subst(ta.get(), g));
        }
        return n;
    }
    case ast::NodeKind::PointerTypeNode: {
        auto *src = static_cast<const ast::PointerTypeNode *>(t);
        auto p = std::make_unique<ast::PointerTypeNode>();
        p->loc = src->loc;
        p->pointee = clone_type_with_subst(src->pointee.get(), g);
        return p;
    }
    case ast::NodeKind::ArrayTypeNode: {
        auto *src = static_cast<const ast::ArrayTypeNode *>(t);
        auto a = std::make_unique<ast::ArrayTypeNode>();
        a->loc = src->loc;
        a->element_type = clone_type_with_subst(src->element_type.get(), g);
        if (src->size_expr) a->size_expr = clone_expr(src->size_expr.get(), g);
        return a;
    }
    default: return nullptr;
    }
}

std::unique_ptr<ast::Expr> clone_expr(const ast::Expr *e,
                                             const GenSubst &g) {
    if (!e) return nullptr;
    switch (e->kind) {
    case ast::NodeKind::IntLitExpr: {
        auto *s = static_cast<const ast::IntLitExpr *>(e);
        auto x = std::make_unique<ast::IntLitExpr>();
        x->loc = s->loc;
        x->value = s->value;
        return x;
    }
    case ast::NodeKind::FloatLitExpr: {
        auto *s = static_cast<const ast::FloatLitExpr *>(e);
        auto x = std::make_unique<ast::FloatLitExpr>();
        x->loc = s->loc;
        x->value = s->value;
        return x;
    }
    case ast::NodeKind::BoolLitExpr: {
        auto *s = static_cast<const ast::BoolLitExpr *>(e);
        auto x = std::make_unique<ast::BoolLitExpr>();
        x->loc = s->loc;
        x->value = s->value;
        return x;
    }
    case ast::NodeKind::NullLitExpr: {
        auto x = std::make_unique<ast::NullLitExpr>();
        x->loc = e->loc;
        return x;
    }
    case ast::NodeKind::CharLitExpr: {
        auto *s = static_cast<const ast::CharLitExpr *>(e);
        auto x = std::make_unique<ast::CharLitExpr>();
        x->loc = s->loc;
        x->codepoint = s->codepoint;
        return x;
    }
    case ast::NodeKind::StringLitExpr: {
        auto *s = static_cast<const ast::StringLitExpr *>(e);
        auto x = std::make_unique<ast::StringLitExpr>();
        x->loc = s->loc;
        x->value = s->value;
        return x;
    }
    case ast::NodeKind::IdentExpr: {
        auto *s = static_cast<const ast::IdentExpr *>(e);
        auto x = std::make_unique<ast::IdentExpr>();
        x->loc = s->loc;
        x->name = s->name;
        return x;
    }
    case ast::NodeKind::ThisExpr: {
        auto x = std::make_unique<ast::ThisExpr>();
        x->loc = e->loc;
        return x;
    }
    case ast::NodeKind::FieldAccessExpr: {
        auto *s = static_cast<const ast::FieldAccessExpr *>(e);
        auto x = std::make_unique<ast::FieldAccessExpr>();
        x->loc = s->loc;
        x->base = clone_expr(s->base.get(), g);
        x->field_name = s->field_name;
        return x;
    }
    case ast::NodeKind::BinaryExpr: {
        auto *s = static_cast<const ast::BinaryExpr *>(e);
        auto x = std::make_unique<ast::BinaryExpr>();
        x->loc = s->loc;
        x->op = s->op;
        x->lhs = clone_expr(s->lhs.get(), g);
        x->rhs = clone_expr(s->rhs.get(), g);
        return x;
    }
    case ast::NodeKind::UnaryExpr: {
        auto *s = static_cast<const ast::UnaryExpr *>(e);
        auto x = std::make_unique<ast::UnaryExpr>();
        x->loc = s->loc;
        x->op = s->op;
        x->operand = clone_expr(s->operand.get(), g);
        return x;
    }
    case ast::NodeKind::CastExpr: {
        auto *s = static_cast<const ast::CastExpr *>(e);
        auto x = std::make_unique<ast::CastExpr>();
        x->loc = s->loc;
        x->target_type = clone_type_with_subst(s->target_type.get(), g);
        x->operand = clone_expr(s->operand.get(), g);
        return x;
    }
    case ast::NodeKind::AssignExpr: {
        auto *s = static_cast<const ast::AssignExpr *>(e);
        auto x = std::make_unique<ast::AssignExpr>();
        x->loc = s->loc;
        x->op = s->op;
        x->target = clone_expr(s->target.get(), g);
        x->value = clone_expr(s->value.get(), g);
        return x;
    }
    case ast::NodeKind::CallExpr: {
        auto *s = static_cast<const ast::CallExpr *>(e);
        auto x = std::make_unique<ast::CallExpr>();
        x->loc = s->loc;
        x->callee = clone_expr(s->callee.get(), g);
        for (auto &a : s->args)
            x->args.push_back(clone_expr(a.get(), g));
        return x;
    }
    case ast::NodeKind::IndexExpr: {
        auto *s = static_cast<const ast::IndexExpr *>(e);
        auto x = std::make_unique<ast::IndexExpr>();
        x->loc = s->loc;
        x->base = clone_expr(s->base.get(), g);
        x->index = clone_expr(s->index.get(), g);
        x->is_range = s->is_range;
        x->range_inclusive = s->range_inclusive;
        if (s->range_hi) x->range_hi = clone_expr(s->range_hi.get(), g);
        return x;
    }
    case ast::NodeKind::NewExpr: {
        auto *s = static_cast<const ast::NewExpr *>(e);
        auto x = std::make_unique<ast::NewExpr>();
        x->loc = s->loc;
        // BugFix P1-A1: si class_name matchea un type-param,
        // substituir al tipo concreto (e.g. `new T[cap]` -> `new i32[cap]`).
        // Sin esto el monomorphized clone tenia `new T[cap]` literal y
        // el type checker reportaba "tipo desconocido 'T'".  Si el
        // arg es CLASS/STRUCT/ENUM, usa el struct_name.  Si es
        // primitivo, usa el nombre canonico ("i32", "f64", etc).
        bool substituted = false;
        if (g.params && g.args) {
            for (size_t i = 0; i < g.params->size(); ++i) {
                if ((*g.params)[i] == s->class_name) {
                    const Type &a = (*g.args)[i];
                    if (a.kind == PrimitiveKind::CLASS ||
                        a.kind == PrimitiveKind::STRUCT) {
                        x->class_name = a.struct_name;
                    } else {
                        x->class_name = type_to_string(a);
                    }
                    substituted = true;
                    break;
                }
            }
        }
        if (!substituted) x->class_name = s->class_name;
        for (auto &a : s->args)
            x->args.push_back(clone_expr(a.get(), g));
        for (auto &t : s->type_args)
            x->type_args.push_back(clone_type_with_subst(t.get(), g));
        if (s->array_size) x->array_size = clone_expr(s->array_size.get(), g);
        return x;
    }
    case ast::NodeKind::TernaryExpr: {
        auto *s = static_cast<const ast::TernaryExpr *>(e);
        auto x = std::make_unique<ast::TernaryExpr>();
        x->loc = s->loc;
        x->cond = clone_expr(s->cond.get(), g);
        x->then_expr = clone_expr(s->then_expr.get(), g);
        x->else_expr = clone_expr(s->else_expr.get(), g);
        return x;
    }
    case ast::NodeKind::TryExpr: {
        auto *s = static_cast<const ast::TryExpr *>(e);
        auto x = std::make_unique<ast::TryExpr>();
        x->loc = s->loc;
        x->operand = clone_expr(s->operand.get(), g);
        return x;
    }
    case ast::NodeKind::MatchExpr: {
        auto *s = static_cast<const ast::MatchExpr *>(e);
        auto x = std::make_unique<ast::MatchExpr>();
        x->loc = s->loc;
        x->scrutinee = clone_expr(s->scrutinee.get(), g);
        for (const auto &arm : s->arms) {
            ast::MatchArm na;
            na.loc = arm.loc;
            na.variant_name = arm.variant_name;
            na.bindings = arm.bindings;
            if (arm.body) na.body = clone_stmt(arm.body.get(), g);
            if (arm.guard) na.guard = clone_expr(arm.guard.get(), g);
            x->arms.push_back(std::move(na));
        }
        return x;
    }
    case ast::NodeKind::InitListExpr: {
        auto *s = static_cast<const ast::InitListExpr *>(e);
        auto x = std::make_unique<ast::InitListExpr>();
        x->loc = s->loc;
        x->is_designated = s->is_designated;
        x->field_names = s->field_names;
        x->target_type_name = s->target_type_name;
        for (auto &el : s->elements)
            x->elements.push_back(clone_expr(el.get(), g));
        return x;
    }
    case ast::NodeKind::LambdaExpr: {
        auto *s = static_cast<const ast::LambdaExpr *>(e);
        auto x = std::make_unique<ast::LambdaExpr>();
        x->loc = s->loc;
        for (const auto &p : s->params) {
            auto np = std::make_unique<ast::ParamDecl>();
            np->loc = p->loc;
            np->name = p->name;
            np->type = clone_type_with_subst(p->type.get(), g);
            x->params.push_back(std::move(np));
        }
        if (s->return_type)
            x->return_type = clone_type_with_subst(s->return_type.get(), g);
        if (s->body) {
            auto cb = clone_stmt(s->body.get(), g);
            if (cb && cb->kind == ast::NodeKind::BlockStmt)
                x->body.reset(static_cast<ast::BlockStmt *>(cb.release()));
        }
        return x;
    }
    case ast::NodeKind::SpawnExpr: {
        auto *s = static_cast<const ast::SpawnExpr *>(e);
        auto x = std::make_unique<ast::SpawnExpr>();
        x->loc = s->loc;
        x->policy = s->policy;
        if (s->sched_idx) x->sched_idx = clone_expr(s->sched_idx.get(), g);
        if (s->body) {
            auto cb = clone_stmt(s->body.get(), g);
            if (cb && cb->kind == ast::NodeKind::BlockStmt)
                x->body.reset(static_cast<ast::BlockStmt *>(cb.release()));
        }
        return x;
    }
    case ast::NodeKind::RSpawnExpr: {
        auto *s = static_cast<const ast::RSpawnExpr *>(e);
        auto x = std::make_unique<ast::RSpawnExpr>();
        x->loc = s->loc;
        x->node_idx = clone_expr(s->node_idx.get(), g);
        if (s->body) {
            auto cb = clone_stmt(s->body.get(), g);
            if (cb && cb->kind == ast::NodeKind::BlockStmt)
                x->body.reset(static_cast<ast::BlockStmt *>(cb.release()));
        }
        return x;
    }
    default: return nullptr;
    }
}

std::unique_ptr<ast::Stmt> clone_stmt(const ast::Stmt *s,
                                             const GenSubst &g) {
    if (!s) return nullptr;
    switch (s->kind) {
    case ast::NodeKind::BlockStmt: {
        auto *src = static_cast<const ast::BlockStmt *>(s);
        auto x = std::make_unique<ast::BlockStmt>();
        x->loc = src->loc;
        for (auto &b : src->body)
            x->body.push_back(clone_stmt(b.get(), g));
        return x;
    }
    case ast::NodeKind::VarDeclStmt: {
        auto *src = static_cast<const ast::VarDeclStmt *>(s);
        auto x = std::make_unique<ast::VarDeclStmt>();
        x->loc = src->loc;
        x->name = src->name;
        x->is_const = src->is_const;
        x->reg_binding = src->reg_binding; // Phase AS inc.2
        x->type = clone_type_with_subst(src->type.get(), g);
        if (src->init) x->init = clone_expr(src->init.get(), g);
        return x;
    }
    case ast::NodeKind::ExprStmt: {
        auto *src = static_cast<const ast::ExprStmt *>(s);
        auto x = std::make_unique<ast::ExprStmt>();
        x->loc = src->loc;
        x->expr = clone_expr(src->expr.get(), g);
        return x;
    }
    case ast::NodeKind::IfStmt: {
        auto *src = static_cast<const ast::IfStmt *>(s);
        auto x = std::make_unique<ast::IfStmt>();
        x->loc = src->loc;
        x->cond = clone_expr(src->cond.get(), g);
        x->then_branch = clone_stmt(src->then_branch.get(), g);
        x->else_branch = clone_stmt(src->else_branch.get(), g);
        return x;
    }
    case ast::NodeKind::WhileStmt: {
        auto *src = static_cast<const ast::WhileStmt *>(s);
        auto x = std::make_unique<ast::WhileStmt>();
        x->loc = src->loc;
        x->cond = clone_expr(src->cond.get(), g);
        x->body = clone_stmt(src->body.get(), g);
        return x;
    }
    case ast::NodeKind::DoWhileStmt: {
        auto *src = static_cast<const ast::DoWhileStmt *>(s);
        auto x = std::make_unique<ast::DoWhileStmt>();
        x->loc = src->loc;
        x->body = clone_stmt(src->body.get(), g);
        x->cond = clone_expr(src->cond.get(), g);
        return x;
    }
    case ast::NodeKind::ForStmt: {
        auto *src = static_cast<const ast::ForStmt *>(s);
        auto x = std::make_unique<ast::ForStmt>();
        x->loc = src->loc;
        x->init = clone_stmt(src->init.get(), g);
        x->cond = clone_expr(src->cond.get(), g);
        x->step = clone_expr(src->step.get(), g);
        x->body = clone_stmt(src->body.get(), g);
        return x;
    }
    case ast::NodeKind::ReturnStmt: {
        auto *src = static_cast<const ast::ReturnStmt *>(s);
        auto x = std::make_unique<ast::ReturnStmt>();
        x->loc = src->loc;
        if (src->value) x->value = clone_expr(src->value.get(), g);
        return x;
    }
    case ast::NodeKind::BreakStmt: {
        auto x = std::make_unique<ast::BreakStmt>();
        x->loc = s->loc;
        return x;
    }
    case ast::NodeKind::ContinueStmt: {
        auto x = std::make_unique<ast::ContinueStmt>();
        x->loc = s->loc;
        return x;
    }
    case ast::NodeKind::ThrowStmt: {
        auto *src = static_cast<const ast::ThrowStmt *>(s);
        auto x = std::make_unique<ast::ThrowStmt>();
        x->loc = src->loc;
        x->value = clone_expr(src->value.get(), g);
        return x;
    }
    case ast::NodeKind::TryStmt: {
        auto *src = static_cast<const ast::TryStmt *>(s);
        auto x = std::make_unique<ast::TryStmt>();
        x->loc = src->loc;
        if (src->body) {
            auto cb = clone_stmt(src->body.get(), g);
            if (cb && cb->kind == ast::NodeKind::BlockStmt)
                x->body.reset(static_cast<ast::BlockStmt *>(cb.release()));
        }
        for (const auto &c : src->catches) {
            ast::CatchClause nc;
            nc.loc = c.loc;
            nc.exc_class_name = c.exc_class_name;
            nc.var_name = c.var_name;
            if (c.body) {
                auto cb = clone_stmt(c.body.get(), g);
                if (cb && cb->kind == ast::NodeKind::BlockStmt)
                    nc.body.reset(static_cast<ast::BlockStmt *>(cb.release()));
            }
            x->catches.push_back(std::move(nc));
        }
        if (src->finally_body) {
            auto cb = clone_stmt(src->finally_body.get(), g);
            if (cb && cb->kind == ast::NodeKind::BlockStmt)
                x->finally_body.reset(
                    static_cast<ast::BlockStmt *>(cb.release()));
        }
        return x;
    }
    case ast::NodeKind::ForEachStmt: {
        auto *src = static_cast<const ast::ForEachStmt *>(s);
        auto x = std::make_unique<ast::ForEachStmt>();
        x->loc = src->loc;
        x->iter_name = src->iter_name;
        x->iter_type = clone_type_with_subst(src->iter_type.get(), g);
        x->iter_expr = clone_expr(src->iter_expr.get(), g);
        x->body = clone_stmt(src->body.get(), g);
        return x;
    }
    case ast::NodeKind::SynchronizedStmt: {
        auto *src = static_cast<const ast::SynchronizedStmt *>(s);
        auto x = std::make_unique<ast::SynchronizedStmt>();
        x->loc = src->loc;
        x->target = clone_expr(src->target.get(), g);
        if (src->body) {
            auto cb = clone_stmt(src->body.get(), g);
            if (cb && cb->kind == ast::NodeKind::BlockStmt)
                x->body.reset(static_cast<ast::BlockStmt *>(cb.release()));
        }
        return x;
    }
    case ast::NodeKind::GotoStmt: {
        auto *src = static_cast<const ast::GotoStmt *>(s);
        auto x = std::make_unique<ast::GotoStmt>();
        x->loc = src->loc;
        x->label = src->label;
        return x;
    }
    case ast::NodeKind::LabelStmt: {
        auto *src = static_cast<const ast::LabelStmt *>(s);
        auto x = std::make_unique<ast::LabelStmt>();
        x->loc = src->loc;
        x->name = src->name;
        return x;
    }
    case ast::NodeKind::ComptimeBlockStmt: {
        auto *src = static_cast<const ast::ComptimeBlockStmt *>(s);
        auto x = std::make_unique<ast::ComptimeBlockStmt>();
        x->loc = src->loc;
        for (auto &b : src->stmts)
            x->stmts.push_back(clone_stmt(b.get(), g));
        return x;
    }
    case ast::NodeKind::ComptimeForStmt: {
        auto *src = static_cast<const ast::ComptimeForStmt *>(s);
        auto x = std::make_unique<ast::ComptimeForStmt>();
        x->loc = src->loc;
        x->var_name = src->var_name;
        x->inclusive = src->inclusive;
        x->lo_expr = clone_expr(src->lo_expr.get(), g);
        x->hi_expr = clone_expr(src->hi_expr.get(), g);
        x->body = clone_stmt(src->body.get(), g);
        return x;
    }
    default: return nullptr;
    }
}

} // namespace vexgen
} // namespace vex
