/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file diagram_labels.cpp
 * @brief Implementacion de las etiquetas de un diagrama.
 *
 * Ver @c vx/diagram/diagram_labels.h para el motivo de que esto viva aparte de
 * los dos generadores.
 */

#include "vx/diagram/diagram_labels.h"

#include <sstream>
#include <string>

namespace vx {

const char *binop_symbol(ast::BinOp op) {
    switch (op) {
    case ast::BinOp::Add: return "+";
    case ast::BinOp::Sub: return "-";
    case ast::BinOp::Mul: return "*";
    case ast::BinOp::Div: return "/";
    case ast::BinOp::Mod: return "%";
    case ast::BinOp::Eq: return "==";
    case ast::BinOp::Neq: return "!=";
    case ast::BinOp::Lt: return "<";
    case ast::BinOp::Gt: return ">";
    case ast::BinOp::Le: return "<=";
    case ast::BinOp::Ge: return ">=";
    case ast::BinOp::LogicalAnd: return "&&";
    case ast::BinOp::LogicalOr: return "||";
    case ast::BinOp::BitAnd: return "&";
    case ast::BinOp::BitOr: return "|";
    case ast::BinOp::BitXor: return "^";
    case ast::BinOp::Shl: return "<<";
    case ast::BinOp::Shr: return ">>";
    default: return "?";
    }
}

const char *unop_symbol(ast::UnOp op) {
    switch (op) {
    case ast::UnOp::Neg: return "-";
    case ast::UnOp::Pos: return "+";
    case ast::UnOp::LogicalNot: return "!";
    case ast::UnOp::BitNot: return "~";
    case ast::UnOp::AddrOf: return "&";
    case ast::UnOp::Deref: return "*";
    case ast::UnOp::PreInc: return "++";
    case ast::UnOp::PreDec: return "--";
    case ast::UnOp::PostInc: return "(++)";
    case ast::UnOp::PostDec: return "(--)";
    case ast::UnOp::Unwrap: return "!!";
    case ast::UnOp::Await: return "await ";
    default: return "?";
    }
}

const char *assignop_symbol(ast::AssignOp op) {
    switch (op) {
    case ast::AssignOp::Assign: return "=";
    case ast::AssignOp::AddAssign: return "+=";
    case ast::AssignOp::SubAssign: return "-=";
    case ast::AssignOp::MulAssign: return "*=";
    case ast::AssignOp::DivAssign: return "/=";
    case ast::AssignOp::ModAssign: return "%=";
    case ast::AssignOp::BitAndAssign: return "&=";
    case ast::AssignOp::BitOrAssign: return "|=";
    case ast::AssignOp::BitXorAssign: return "^=";
    case ast::AssignOp::ShlAssign: return "<<=";
    case ast::AssignOp::ShrAssign: return ">>=";
    default: return "?=";
    }
}

std::string fmt_expr_brief(const ast::Expr *e, int depth) {
    if (!e) return "?";
    if (depth <= 0) return "...";
    switch (e->kind) {
    case ast::NodeKind::IntLitExpr: {
        auto *l = static_cast<const ast::IntLitExpr *>(e);
        return std::to_string(l->value);
    }
    case ast::NodeKind::FloatLitExpr: {
        auto *l = static_cast<const ast::FloatLitExpr *>(e);
        return std::to_string(l->value);
    }
    case ast::NodeKind::BoolLitExpr: {
        auto *l = static_cast<const ast::BoolLitExpr *>(e);
        return l->value ? "true" : "false";
    }
    case ast::NodeKind::NullLitExpr: return "null";
    case ast::NodeKind::CharLitExpr: {
        auto *l = static_cast<const ast::CharLitExpr *>(e);
        if (l->codepoint < 128 && l->codepoint >= 32) {
            std::string s = "'";
            s += static_cast<char>(l->codepoint);
            s += "'";
            return s;
        }
        return "'\\u" + std::to_string(l->codepoint) + "'";
    }
    case ast::NodeKind::StringLitExpr: {
        auto *l = static_cast<const ast::StringLitExpr *>(e);
        if (l->is_interpolated()) return "\"" + l->value + "${...}\"";
        return "\"" + l->value + "\"";
    }
    case ast::NodeKind::IdentExpr: {
        auto *l = static_cast<const ast::IdentExpr *>(e);
        return l->name;
    }
    case ast::NodeKind::ThisExpr: return "this";
    case ast::NodeKind::FieldAccessExpr: {
        auto *fe = static_cast<const ast::FieldAccessExpr *>(e);
        return fmt_expr_brief(fe->base.get(), depth - 1) + "." + fe->field_name;
    }
    case ast::NodeKind::IndexExpr: {
        auto *ix = static_cast<const ast::IndexExpr *>(e);
        return fmt_expr_brief(ix->base.get(), depth - 1) + "[" +
               fmt_expr_brief(ix->index.get(), depth - 1) + "]";
    }
    case ast::NodeKind::BinaryExpr: {
        auto *b = static_cast<const ast::BinaryExpr *>(e);
        return "(" + fmt_expr_brief(b->lhs.get(), depth - 1) + " " +
               binop_symbol(b->op) + " " +
               fmt_expr_brief(b->rhs.get(), depth - 1) + ")";
    }
    case ast::NodeKind::UnaryExpr: {
        auto *u = static_cast<const ast::UnaryExpr *>(e);
        return std::string(unop_symbol(u->op)) +
               fmt_expr_brief(u->operand.get(), depth - 1);
    }
    case ast::NodeKind::AssignExpr: {
        auto *a = static_cast<const ast::AssignExpr *>(e);
        return fmt_expr_brief(a->target.get(), depth - 1) + " " +
               assignop_symbol(a->op) + " " +
               fmt_expr_brief(a->value.get(), depth - 1);
    }
    case ast::NodeKind::CallExpr: {
        auto *c = static_cast<const ast::CallExpr *>(e);
        std::string s = fmt_expr_brief(c->callee.get(), depth - 1) + "(";
        for (size_t i = 0; i < c->args.size(); ++i) {
            if (i) s += ", ";
            s += fmt_expr_brief(c->args[i].get(), depth - 1);
        }
        s += ")";
        return s;
    }
    case ast::NodeKind::NewExpr: {
        auto *n = static_cast<const ast::NewExpr *>(e);
        std::string s = "new " + n->class_name + "(";
        for (size_t i = 0; i < n->args.size(); ++i) {
            if (i) s += ", ";
            s += fmt_expr_brief(n->args[i].get(), depth - 1);
        }
        s += ")";
        return s;
    }
    case ast::NodeKind::CastExpr: {
        auto *cs = static_cast<const ast::CastExpr *>(e);
        return "(" + fmt_type(cs->target_type.get()) + ")" +
               fmt_expr_brief(cs->operand.get(), depth - 1);
    }
    case ast::NodeKind::SpawnExpr: {
        auto *sp = static_cast<const ast::SpawnExpr *>(e);
        const char *p = (sp->policy == ast::SpawnExpr::Policy::Here) ? " here"
                        : (sp->policy == ast::SpawnExpr::Policy::Pinned)
                            ? " on(...)"
                            : "";
        return std::string("spawn") + p + " { ... }";
    }
    case ast::NodeKind::RSpawnExpr: return "rspawn(...) { ... }";
    case ast::NodeKind::LambdaExpr: return "(...) => ...";
    case ast::NodeKind::MatchExpr: {
        auto *m = static_cast<const ast::MatchExpr *>(e);
        return "match " + fmt_expr_brief(m->scrutinee.get(), depth - 1) +
               " { " + std::to_string(m->arms.size()) + " arms }";
    }
    case ast::NodeKind::InitListExpr: {
        auto *il = static_cast<const ast::InitListExpr *>(e);
        return "{ " + std::to_string(il->elements.size()) + " elements" +
               (il->is_designated ? ", designated" : "") + " }";
    }
    default: return "<expr>";
    }
}

std::string fmt_type(const ast::TypeNode *tn) {
    return fmt_type_helper(tn);
}

std::string fmt_expr(const ast::Expr *e) {
    // Sin truncamiento ni limite de profundidad efectivo: el usuario
    // pidio explicitamente que no se omita ninguna informacion.  Los
    // AST de Vesta son DAGs (no ciclicos); usamos depth=32 por defensa.
    return fmt_expr_brief(e, 32);
}

std::string fmt_type_helper(const ast::TypeNode *tn) {
    if (!tn) return "?";
    switch (tn->kind) {
    case ast::NodeKind::PrimitiveTypeNode: {
        auto *pt = static_cast<const ast::PrimitiveTypeNode *>(tn);
        std::string s = primitive_name(pt->prim);
        if (!pt->type_args.empty()) {
            s += "<";
            for (size_t i = 0; i < pt->type_args.size(); ++i) {
                if (i) s += ",";
                s += fmt_type(pt->type_args[i].get());
            }
            s += ">";
        }
        return s;
    }
    case ast::NodeKind::NamedTypeNode: {
        auto *nt = static_cast<const ast::NamedTypeNode *>(tn);
        std::string s = nt->name;
        if (!nt->type_args.empty()) {
            s += "<";
            for (size_t i = 0; i < nt->type_args.size(); ++i) {
                if (i) s += ",";
                s += fmt_type(nt->type_args[i].get());
            }
            s += ">";
        }
        return s;
    }
    case ast::NodeKind::PointerTypeNode: {
        auto *pn = static_cast<const ast::PointerTypeNode *>(tn);
        if (pn->is_virtual) {
            return "VirtualPtr<" + fmt_type(pn->pointee.get()) + ">";
        }
        return fmt_type(pn->pointee.get()) + "*";
    }
    case ast::NodeKind::ArrayTypeNode: {
        auto *an = static_cast<const ast::ArrayTypeNode *>(tn);
        std::string s = fmt_type(an->element_type.get());
        s += "[";
        if (an->size_expr) s += "N";
        s += "]";
        return s;
    }
    case ast::NodeKind::FunctionTypeNode: {
        auto *ft = static_cast<const ast::FunctionTypeNode *>(tn);
        std::string s = "fn(";
        for (size_t i = 0; i < ft->param_types.size(); ++i) {
            if (i) s += ",";
            s += fmt_type(ft->param_types[i].get());
        }
        s += ") -> ";
        s += fmt_type(ft->return_type.get());
        return s;
    }
    default: return "<unknown_type>";
    }
}

std::string fmt_value_id(const ir::IrFunction &fn, ir::IrValueId id) {
    if (id == ir::IR_NO_VALUE) return "?";
    if (id < fn.values.size()) {
        const auto &v = fn.values[id];
        if (!v.name.empty() && v.name[0] != '%') return "%" + v.name;
        if (!v.name.empty()) return v.name;
    }
    return "%" + std::to_string(id);
}

std::string fmt_instr(const ir::IrFunction &fn, const ir::IrInstr &ins,
                      const std::vector<ir::IrBlock> &blocks) {
    std::ostringstream s;
    const char *opn = ir::ir_op_name(ins.op);

    if (ins.op == ir::IrOp::BR) {
        s << "br -> "
          << (ins.target_block < blocks.size()
                  ? blocks[ins.target_block].name
                  : std::to_string(ins.target_block));
    } else if (ins.op == ir::IrOp::BR_COND) {
        s << "br.cond "
          << fmt_value_id(fn, ins.operands.empty() ? ir::IR_NO_VALUE
                                                   : ins.operands[0])
          << " ? "
          << (ins.target_block < blocks.size() ? blocks[ins.target_block].name
                                               : "?")
          << " : "
          << (ins.false_block < blocks.size() ? blocks[ins.false_block].name
                                              : "?");
    } else if (ins.op == ir::IrOp::RET) {
        if (!ins.operands.empty()) {
            s << "ret " << fmt_value_id(fn, ins.operands[0]);
        } else {
            s << "ret";
        }
    } else if (ins.op == ir::IrOp::UNREACHABLE) {
        s << "unreachable";
    } else if (ins.op == ir::IrOp::PHI) {
        if (ins.dst != ir::IR_NO_VALUE) s << fmt_value_id(fn, ins.dst) << " = ";
        s << "phi";
        // Sin limite: mostrar TODOS los phi args.
        for (const auto &pa : ins.phi_args) {
            s << " [" << fmt_value_id(fn, pa.value) << " from "
              << (pa.block < blocks.size() ? blocks[pa.block].name : "?")
              << "]";
        }
    } else if (ins.op == ir::IrOp::CONST) {
        if (ins.dst != ir::IR_NO_VALUE) s << fmt_value_id(fn, ins.dst) << " = ";
        s << "const " << ins.imm;
    } else if (ins.op == ir::IrOp::CALL || ins.op == ir::IrOp::CALLN ||
               ins.op == ir::IrOp::TAILCALL) {
        if (ins.dst != ir::IR_NO_VALUE) s << fmt_value_id(fn, ins.dst) << " = ";
        s << opn << " " << ins.func_name;
        s << "(" << ins.operands.size() << " args)";
    } else if (ins.op == ir::IrOp::CALLVIRT || ins.op == ir::IrOp::CALLM) {
        if (ins.dst != ir::IR_NO_VALUE) s << fmt_value_id(fn, ins.dst) << " = ";
        s << opn << " (" << ins.operands.size() << " args)";
    } else if (ins.op == ir::IrOp::CALLCLOSURE || ins.op == ir::IrOp::CALLIND) {
        if (ins.dst != ir::IR_NO_VALUE) s << fmt_value_id(fn, ins.dst) << " = ";
        s << opn << " " << fmt_value_id(fn, ins.func_ptr) << "("
          << ins.operands.size() << " args)";
    } else if (ins.op == ir::IrOp::SPAWN_ARGS) {
        if (ins.dst != ir::IR_NO_VALUE) s << fmt_value_id(fn, ins.dst) << " = ";
        s << "spawn_args (" << ins.operands.size() << " operands)";
    } else if (ins.op == ir::IrOp::RAW_ASM) {
        // Sin truncamiento: emitimos el texto raw completo (con
        // saltos de linea sustituidos por `;` para que quepa en
        // una celda del record sin romper el render).
        std::string txt = ins.func_name;
        for (char &c : txt)
            if (c == '\n' || c == '\r') c = ' ';
        s << "raw_asm \"" << txt << "\"";
    } else {
        if (ins.dst != ir::IR_NO_VALUE) s << fmt_value_id(fn, ins.dst) << " = ";
        s << opn;
        // Sin limite en el numero de operandos.
        for (size_t i = 0; i < ins.operands.size(); ++i) {
            if (i == 0)
                s << " ";
            else
                s << ", ";
            s << fmt_value_id(fn, ins.operands[i]);
        }
    }
    if (ins.source_line > 0) {
        s << " (L" << ins.source_line << ")";
    }
    return s.str();
}

} // namespace vx
