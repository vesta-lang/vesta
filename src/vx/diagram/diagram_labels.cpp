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

#include "vx/annotation_names.h" // la grafia de una anotacion, en su tabla
#include "vx/token.h"            // y la de una palabra clave, en el lexico

#include <sstream>
#include <string>

namespace vx {

namespace {

/* Una PALABRA CLAVE se escribe como la escribe el lexico, y una ANOTACION como
 * la nombra su tabla.  Ninguna de las dos se vuelve a teclear aqui: son los dos
 * vocabularios del lenguaje y ya tienen dueno (@c token_kind_name y
 * @c vx/annotation_names.h), asi que una segunda grafia no daria un error --
 * daria un diagrama que dice otra cosa que el compilador. */
inline void add_keyword(std::string &s, TokenKind k) {
    s += token_kind_name(k);
    s += ' ';
}

/// `@Nombre ` -- la anotacion tal y como se escribe.
inline void add_annotation(std::string &s, const char *name) {
    s += '@';
    s += name;
    s += ' ';
}

/// `@Nombre("objetivo") ` -- una anotacion de aspecto, con su pointcut.
inline void add_advice(std::string &s, const char *name,
                       const std::string &target) {
    s += '@';
    s += name;
    s += "(\"";
    s += target;
    s += "\") ";
}

/* Lo que NO es vocabulario del lenguaje sino de este dibujo: separadores y
 * rotulos.  Aqui si es una constante porque aqui es donde nace. */
constexpr const char *kCtor = "<ctor> ";
constexpr const char *kDtor = "<dtor> ";
constexpr const char *kArrow = ") -> ";
constexpr const char *kSep = ", ";
constexpr const char *kAssign = " = ";
constexpr const char *kOpenParen = " (";
constexpr const char *kFieldsSep = " fields, ";
constexpr const char *kMethods = " methods";
constexpr const char *kFieldsEnd = " fields)";
constexpr const char *kVariantsEnd = " variants)";
constexpr const char *kBitWidth = " : ";

} // namespace

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
        std::string out = fmt_expr_brief(ix->base.get(), depth - 1) + "[";
        if (!ix->is_range)
            return out + fmt_expr_brief(ix->index.get(), depth - 1) + "]";
        /* Un rango se dibuja como se escribio: el limite que falta se deja en
         * blanco (`[..b]`, `[a..]`), no con el "?" de lo desconocido. */
        if (ix->index) out += fmt_expr_brief(ix->index.get(), depth - 1);
        out += ix->range_inclusive ? "..=" : "..";
        if (ix->range_hi) out += fmt_expr_brief(ix->range_hi.get(), depth - 1);
        return out + "]";
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

std::string fmt_expr_list(const std::vector<std::unique_ptr<ast::Expr>> &list) {
    std::string out;
    for (const auto &e : list) {
        if (!out.empty()) out += ", ";
        out += fmt_expr(e.get());
    }
    return out;
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
        /* `cfn` Y `fn` SON TIPOS DISTINTOS, y el nodo lo sabe (@c is_raw): uno
         * es una direccion de ocho bytes con llamada directa, el otro un par
         * {funcion, entorno} de dieciseis.  Escribiendo los dos como `fn`, dos
         * firmas que NO se pueden intercambiar se leian identicas -- el caso
         * que lo enseno son `aplica(cfn(i64)->i64)` y
         * `aplica_lam(fn(i64)->i64)` saliendo iguales.
         *
         * El variadico igual: `fn(T...)` acepta cuantos le echen y `fn(T)` uno,
         * y sin los puntos son la misma linea. */
        std::string s = ft->is_raw ? "cfn(" : "fn(";
        for (size_t i = 0; i < ft->param_types.size(); ++i) {
            if (i) s += ",";
            s += fmt_type(ft->param_types[i].get());
            if (ft->is_variadic && i + 1 == ft->param_types.size()) s += "...";
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

std::unordered_map<std::string, uint32_t>
class_index_by_name(const ast::ModuleNode &mod) {
    std::unordered_map<std::string, uint32_t> out;
    /* El NUMERO, no el nombre del nodo: cada formato le pone su prefijo, que
     * ya sabe.  Guardar aqui la cadena "C_0" seria una reserva por clase para
     * escribir dos caracteres que el que dibuja escribe igual. */
    uint32_t at = 0;
    for (const auto &dn : mod.decls) {
        if (!dn || dn->kind != ast::NodeKind::ClassDecl) continue;
        out.emplace(static_cast<const ast::ClassDecl *>(dn.get())->name, at);
        ++at;
    }
    return out;
}

ClassBases
class_bases(const ast::ClassDecl &cd,
            const std::unordered_map<std::string, uint32_t> &classes) {
    ClassBases out;
    if (!cd.super_name.empty()) {
        const auto at = classes.find(cd.super_name);
        if (at != classes.end()) {
            out.super = at->second;
            out.super_is_local = true;
        }
    }
    /* Una interfaz declarada aqui tiene su propio nodo, asi que la arista va a
     * el; las de fuera siguen consolidandose en una lista, que es lo que eran
     * antes TODAS: hojas sin estructura propia. */
    for (const auto &iref : cd.interface_names) {
        // El nombre internado vive en el pozo: su direccion es estable.
        const std::string &iname = iref.name.str();
        const auto at = classes.find(iname);
        if (at != classes.end())
            out.local_ifaces.push_back(at->second);
        else
            out.foreign.push_back(&iname);
    }
    return out;
}

size_t count_stmts(const ast::Stmt *s) {
    if (!s) return 0;
    if (s->kind == ast::NodeKind::BlockStmt) {
        auto *bs = static_cast<const ast::BlockStmt *>(s);
        size_t total = 0;
        for (const auto &st : bs->body)
            total += count_stmts(st.get());
        return total > 0 ? total : 1;
    }
    return 1;
}

std::string
fmt_params(const std::vector<std::unique_ptr<ast::ParamDecl>> &params) {
    std::string s;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) s += kSep;
        s += fmt_type(params[i]->type.get());
        s += ' ';
        s += params[i]->name;
    }
    return s;
}

std::string fmt_function_signature(const ast::FunctionDecl &fd) {
    std::string s;
    if (fd.is_async) add_annotation(s, ann::kAsync);
    add_keyword(s, TokenKind::KW_FN);
    s += fd.name;
    s += '(';
    s += fmt_params(fd.params);
    s += kArrow;
    s += fmt_type(fd.return_type.get());
    return s;
}

std::string fmt_body_info(const ast::BlockStmt *body, const char *prefix,
                          const char *suffix, const char *when_empty) {
    if (!body) return when_empty;
    std::string s = prefix;
    s += std::to_string(count_stmts(body));
    s += suffix;
    return s;
}

std::string fmt_class_title(const ast::ClassDecl &cd) {
    std::string s;
    if (cd.is_interface) {
        add_keyword(s, TokenKind::KW_INTERFACE);
    } else {
        if (cd.is_aspect) add_annotation(s, ann::kAspect);
        if (cd.is_final) add_keyword(s, TokenKind::KW_FINAL);
        add_keyword(s, TokenKind::KW_CLASS);
    }
    s += cd.name;
    if (!cd.type_params.empty()) {
        s += '<';
        for (size_t i = 0; i < cd.type_params.size(); ++i) {
            if (i) s += ',';
            s += cd.type_params[i];
        }
        s += '>';
    }
    return s;
}

std::string fmt_class_summary(const ast::ClassDecl &cd) {
    std::string s = std::to_string(cd.fields.size());
    s += kFieldsSep;
    s += std::to_string(cd.methods.size());
    s += kMethods;
    return s;
}

/// La palabra del acceso, que siempre se escribe.
static TokenKind access_keyword(uint8_t access) {
    return access == 1   ? TokenKind::KW_PRIVATE
           : access == 2 ? TokenKind::KW_PROTECTED
                         : TokenKind::KW_PUBLIC;
}

std::string fmt_field_line(const ast::ClassFieldDecl &f) {
    std::string s;
    add_keyword(s, access_keyword(f.access));
    if (f.is_static) add_keyword(s, TokenKind::KW_STATIC);
    if (f.is_final) add_keyword(s, TokenKind::KW_FINAL);
    s += fmt_type(f.type.get());
    s += ' ';
    s += f.name;
    return s;
}

std::string fmt_method_signature(const ast::ClassMethodDecl &m) {
    std::string s;
    add_keyword(s, access_keyword(m.access));
    if (m.is_static) add_keyword(s, TokenKind::KW_STATIC);
    if (m.is_final) add_keyword(s, TokenKind::KW_FINAL);
    if (m.is_override) add_annotation(s, ann::kOverride);
    if (m.is_constructor) s += kCtor;
    if (m.is_destructor) s += kDtor;
    if (m.is_inline) add_annotation(s, ann::kInline);
    // Los tres aspectos se escriben igual salvo la palabra.
    const char *advice = m.advice_kind == 1   ? ann::kBefore
                         : m.advice_kind == 2 ? ann::kAfter
                         : m.advice_kind == 3 ? ann::kAround
                                              : nullptr;
    if (advice) add_advice(s, advice, m.advice_target);
    s += m.return_type ? fmt_type(m.return_type.get())
                       : std::string(token_kind_name(TokenKind::KW_VOID));
    s += ' ';
    s += m.name;
    s += '(';
    s += fmt_params(m.params);
    s += ')';
    return s;
}

std::string fmt_struct_title(const ast::StructDecl &sd) {
    std::string s;
    add_keyword(s, TokenKind::KW_STRUCT);
    s += sd.name;
    s += kOpenParen;
    s += std::to_string(sd.fields.size());
    s += kFieldsEnd;
    return s;
}

std::string fmt_struct_field_line(const ast::StructFieldDecl &f) {
    std::string s = fmt_type(f.type.get());
    s += ' ';
    s += f.name;
    if (f.bit_width > 0) {
        s += kBitWidth;
        s += std::to_string(f.bit_width);
    }
    return s;
}

std::string fmt_enum_title(const ast::EnumDecl &ed) {
    std::string s;
    add_keyword(s, TokenKind::KW_ENUM);
    s += ed.name;
    s += kOpenParen;
    s += std::to_string(ed.variants.size());
    s += kVariantsEnd;
    return s;
}

std::string fmt_enum_variant_line(const ast::EnumVariantDecl &v) {
    std::string s = v.name;
    if (v.field_types.empty()) return s;
    s += '(';
    for (size_t i = 0; i < v.field_types.size(); ++i) {
        if (i) s += kSep;
        s += fmt_type(v.field_types[i].get());
    }
    s += ')';
    return s;
}

std::string fmt_global_line(const ast::GlobalVarDecl &gv) {
    std::string s;
    if (gv.is_const) add_keyword(s, TokenKind::KW_CONST);
    s += fmt_type(gv.type.get());
    s += ' ';
    s += gv.name;
    if (gv.init) {
        s += kAssign;
        s += fmt_expr_brief(gv.init.get(), 32);
    }
    return s;
}

StmtFusion classify_stmt_for_fusion(const ast::Stmt *s) {
    StmtFusion r;
    if (!s) return r;
    switch (s->kind) {
    case ast::NodeKind::VarDeclStmt: {
        auto *v = static_cast<const ast::VarDeclStmt *>(s);
        std::string lbl;
        if (v->is_const) add_keyword(lbl, TokenKind::KW_CONST);
        lbl += fmt_type(v->type.get());
        lbl += ' ';
        lbl += v->name;
        if (v->init) {
            lbl += kAssign;
            lbl += fmt_expr(v->init.get());
        }
        r.group = "var";
        r.line = std::move(lbl);
        r.style = StmtStyle::Var;
        return r;
    }
    case ast::NodeKind::ExprStmt: {
        auto *e = static_cast<const ast::ExprStmt *>(s);
        if (!e->expr) {
            r.group = "noop";
            r.line = "(no-op)";
            r.style = StmtStyle::Aux;
            return r;
        }
        auto *expr = e->expr.get();
        r.line = fmt_expr(expr);
        if (expr->kind == ast::NodeKind::CallExpr ||
            expr->kind == ast::NodeKind::NewExpr) {
            r.group = "call";
            r.style = StmtStyle::Call;
        } else if (expr->kind == ast::NodeKind::SpawnExpr ||
                   expr->kind == ast::NodeKind::RSpawnExpr) {
            r.group = "spawn";
            r.style = StmtStyle::Spawn;
        } else if (expr->kind == ast::NodeKind::AssignExpr) {
            r.group = "assign";
            r.style = StmtStyle::Assign;
        } else {
            r.group = "expr";
            r.style = StmtStyle::Expr;
        }
        return r;
    }
    default: return r;
    }
}

std::string fmt_extern_line(const ast::ExternFnDecl &ef) {
    std::string s = fmt_type(ef.return_type.get());
    s += ' ';
    s += ef.name;
    s += '(';
    s += fmt_params(ef.params);
    s += ')';
    return s;
}

} // namespace vx
