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

#include "vx/generics/generic_clone.h"

#include "util/os/thread_slot.h" // el mapa activo, sin `thread_local`

namespace vx {
namespace vxgen {

std::string mangle_args(const std::vector<Type> &args) {
    std::string s;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) s += "_";
        s += mangle_type(args[i]);
    }
    return s;
}

std::string generic_instance_name(const std::string &tmpl,
                                  const std::vector<Type> &args) {
    std::string out = tmpl;
    for (char &c : out)
        if (c == '.') c = '_';
    out += '_';
    out += mangle_args(args);
    return out;
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
        // Incluir el pointee + la naturaleza (virtual vs host) para no
        // colisionar
        // (`Caja<i64*>` vs `Caja<u8*>` vs `Caja<VirtualPtr<i64>>`).
        const std::string base = t.is_virtual ? "vptr" : "ptr";
        return t.pointee ? (base + mangle_type(*t.pointee)) : base;
    }
    case PrimitiveKind::ARRAY:
        return t.pointee ? ("arr" + mangle_type(*t.pointee)) : "arr";
    case PrimitiveKind::CLASS:
    case PrimitiveKind::STRUCT: return t.struct_name;
    case PrimitiveKind::STRING: return "str";
    case PrimitiveKind::VOID: return "void";
    case PrimitiveKind::FUNCTION: {
        /* La FIRMA entera y si lleva entorno, por la misma razon que el
         * puntero incluye a que apunta: si no, TODAS las funciones mangleaban
         * igual -- caian en el caso general, que da `x` -- y dos
         * instanciaciones que solo se diferencian en la funcion que reciben
         * salian con la MISMA etiqueta.  La segunda se daba por ya generada y
         * las llamadas acababan en la primera, o sea ejecutando otra funcion
         * que la escrita. */
        std::string s = t.fn_is_raw ? "cfn" : "fn";
        for (const Type &p : t.fn_params()) {
            s += "_";
            s += mangle_type(p);
        }
        s += "_to_";
        s += t.pointee ? mangle_type(*t.pointee) : "void";
        return s;
    }
    default: return "x";
    }
}

// Reconstruye un TypeNode AST a partir de un Type ya resuelto.  Lo usa la
// sustitucion de type-params cuando el arg NO es un escalar simple: un puntero
// (`i64*`) o un array (`i64[4]`) deben preservar su pointee/element y tamano,
// no colapsar a un PrimitiveTypeNode{PTR/ARRAY} que pierde esa info (#2).
std::unique_ptr<ast::TypeNode> type_node_from_type(const Type &a,
                                                   const SourceLoc &loc) {
    // Enum con valor de backing entero/float/string: el kind es el del backing
    // pero la IDENTIDAD del tipo es el nombre del enum (con is_valued_enum).
    // Reconstruir como NamedTypeNode con ese nombre para que la re-resolucion
    // recupere el flag is_valued_enum (si colapsara al primitivo, un
    // `concept<T: Enum>` o `is_enum<T>()` con T=enum perderia la identidad).
    if (a.is_valued_enum && !a.struct_name.empty()) {
        auto n = std::make_unique<ast::NamedTypeNode>();
        n->loc = loc;
        n->name = a.struct_name;
        return n;
    }
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
    case PrimitiveKind::FUNCTION: {
        /* Una funcion se reconstruye ENTERA: sus parametros, su retorno y si
         * es `cfn` o `fn`.  Cayendo en el caso general salia un primitivo
         * pelado -- `fn() -> ?`, sin parametros y sin retorno --, asi que
         * pasar una funcion a un parametro de tipo suelto (`f<T, F>(T x, F
         * f)`) fallaba al comprobar el argumento contra un tipo que no era el
         * que se habia deducido.  Es lo que obligaba a escribir la generica
         * fijando `cfn(T) -> R` -- y entonces una lambda ya no valia. */
        auto fnode = std::make_unique<ast::FunctionTypeNode>();
        fnode->loc = loc;
        fnode->is_raw = a.fn_is_raw;
        fnode->is_variadic = a.fn_is_variadic;
        fnode->param_types.reserve(a.fn_params().size());
        for (const Type &pt : a.fn_params())
            fnode->param_types.push_back(type_node_from_type(pt, loc));
        /* El retorno NUNCA es nulo: el parser mete `void` cuando se omite la
         * flecha, y quien lee el nodo cuenta con eso. */
        fnode->return_type =
            a.pointee ? type_node_from_type(*a.pointee, loc)
                      : type_node_from_type(Type{PrimitiveKind::VOID}, loc);
        return fnode;
    }
    default: {
        auto p = std::make_unique<ast::PrimitiveTypeNode>();
        p->loc = loc;
        p->prim = a.kind;
        return p;
    }
    }
}

// El clon del arbol (clone_type_with_subst, clone_expr, clone_stmt...) vive en
// generic_clone_tree.cpp, sobre la lista de campos de cada nodo.

// ---------------------------------------------------------------------------
//  rename_idents: reescritura in-place de referencias por nombre.
//
//  Recorre sin clonar (a diferencia del resto del fichero): el arbol ya es del
//  llamante y solo cambian cadenas.  Los nodos que no tienen sub-expresiones
//  caen al `default` -- un kind nuevo no rompe nada, simplemente no se recorre.
// ---------------------------------------------------------------------------
namespace {

/* Estado del renombrado: el mapa activo durante el recorrido.
 *
 * En una RANURA y no en `thread_local`: en MinGW la TLS es emulada y cada
 * acceso es una llamada -- y esto se mira una vez por identificador del arbol
 * clonado --.  Lo que se guarda es un puntero, asi que cabe tal cual. */
util::ThreadSlot g_renames_slot;

/// El mapa de renombrado activo en ESTE hilo, o nulo si no hay recorrido.
inline const std::unordered_map<std::string, std::string> *g_renames() {
    return static_cast<const std::unordered_map<std::string, std::string> *>(
        g_renames_slot.get());
}

/// Fija el mapa activo de ESTE hilo.
inline void
set_g_renames(const std::unordered_map<std::string, std::string> *m) {
    g_renames_slot.ensure();
    g_renames_slot.set(const_cast<void *>(static_cast<const void *>(m)));
}

void rename_in_expr(ast::Expr *e);

void rename_in_stmt(ast::Stmt *s);

/// Aplica el mapa a un identificador suelto.
void rename_name(std::string &nm) {
    const auto *const m = g_renames();
    if (!m) return;
    auto it = m->find(nm);
    if (it != m->end()) nm = it->second;
}

void rename_in_expr(ast::Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case ast::NodeKind::IdentExpr:
        rename_name(static_cast<ast::IdentExpr *>(e)->name);
        return;
    case ast::NodeKind::CallExpr: {
        auto *c = static_cast<ast::CallExpr *>(e);
        rename_in_expr(c->callee.get());
        for (auto &a : c->args)
            rename_in_expr(a.get());
        return;
    }
    case ast::NodeKind::BinaryExpr: {
        auto *b = static_cast<ast::BinaryExpr *>(e);
        rename_in_expr(b->lhs.get());
        rename_in_expr(b->rhs.get());
        return;
    }
    case ast::NodeKind::UnaryExpr:
        rename_in_expr(static_cast<ast::UnaryExpr *>(e)->operand.get());
        return;
    case ast::NodeKind::AssignExpr: {
        auto *a = static_cast<ast::AssignExpr *>(e);
        rename_in_expr(a->target.get());
        rename_in_expr(a->value.get());
        return;
    }
    case ast::NodeKind::FieldAccessExpr:
        // Solo la base: `x.campo` renombra `x`, nunca `campo`.
        rename_in_expr(static_cast<ast::FieldAccessExpr *>(e)->base.get());
        return;
    case ast::NodeKind::IndexExpr: {
        // Todas las que cuelgan, tambien el limite superior de un rango.
        for (ast::Expr *sub : static_cast<ast::IndexExpr *>(e)->operands())
            rename_in_expr(sub);
        return;
    }
    case ast::NodeKind::CastExpr:
        rename_in_expr(static_cast<ast::CastExpr *>(e)->operand.get());
        return;
    case ast::NodeKind::TernaryExpr: {
        auto *t = static_cast<ast::TernaryExpr *>(e);
        rename_in_expr(t->cond.get());
        rename_in_expr(t->then_expr.get());
        rename_in_expr(t->else_expr.get());
        return;
    }
    case ast::NodeKind::InitListExpr:
        for (auto &el : static_cast<ast::InitListExpr *>(e)->elements)
            rename_in_expr(el.get());
        return;
    case ast::NodeKind::StringLitExpr:
        for (auto &ie : static_cast<ast::StringLitExpr *>(e)->interp_exprs)
            rename_in_expr(ie.get());
        return;
    case ast::NodeKind::NewExpr:
        for (auto &a : static_cast<ast::NewExpr *>(e)->args)
            rename_in_expr(a.get());
        return;
    case ast::NodeKind::LambdaExpr:
        rename_in_stmt(static_cast<ast::LambdaExpr *>(e)->body.get());
        return;
    case ast::NodeKind::TryExpr:
        rename_in_expr(static_cast<ast::TryExpr *>(e)->operand.get());
        return;
    case ast::NodeKind::MatchExpr: {
        auto *m = static_cast<ast::MatchExpr *>(e);
        rename_in_expr(m->scrutinee.get());
        for (auto &arm : m->arms) {
            rename_in_expr(arm.value_pattern.get());
            rename_in_expr(arm.value_pattern_hi.get());
            rename_in_expr(arm.guard.get());
            rename_in_stmt(arm.body.get());
        }
        return;
    }
    default: return; // literales y nodos sin sub-expresiones
    }
}

void rename_in_stmt(ast::Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case ast::NodeKind::BlockStmt:
        for (auto &c : static_cast<ast::BlockStmt *>(s)->body)
            rename_in_stmt(c.get());
        return;
    case ast::NodeKind::VarDeclStmt:
        rename_in_expr(static_cast<ast::VarDeclStmt *>(s)->init.get());
        return;
    case ast::NodeKind::ExprStmt:
        rename_in_expr(static_cast<ast::ExprStmt *>(s)->expr.get());
        return;
    case ast::NodeKind::ReturnStmt:
        rename_in_expr(static_cast<ast::ReturnStmt *>(s)->value.get());
        return;
    case ast::NodeKind::IfStmt: {
        auto *i = static_cast<ast::IfStmt *>(s);
        rename_in_expr(i->cond.get());
        rename_in_stmt(i->then_branch.get());
        rename_in_stmt(i->else_branch.get());
        return;
    }
    case ast::NodeKind::WhileStmt: {
        auto *w = static_cast<ast::WhileStmt *>(s);
        rename_in_expr(w->cond.get());
        rename_in_stmt(w->body.get());
        return;
    }
    case ast::NodeKind::DoWhileStmt: {
        auto *d = static_cast<ast::DoWhileStmt *>(s);
        rename_in_stmt(d->body.get());
        rename_in_expr(d->cond.get());
        return;
    }
    case ast::NodeKind::ForStmt: {
        auto *f = static_cast<ast::ForStmt *>(s);
        for (auto &in : f->init)
            rename_in_stmt(in.get());
        rename_in_expr(f->cond.get());
        for (auto &st : f->step)
            rename_in_expr(st.get());
        rename_in_stmt(f->body.get());
        return;
    }
    case ast::NodeKind::ForEachStmt: {
        auto *f = static_cast<ast::ForEachStmt *>(s);
        rename_in_expr(f->iter_expr.get());
        rename_in_stmt(f->body.get());
        return;
    }
    case ast::NodeKind::TryStmt: {
        auto *t = static_cast<ast::TryStmt *>(s);
        rename_in_stmt(t->body.get());
        for (auto &c : t->catches)
            rename_in_stmt(c.body.get());
        rename_in_stmt(t->finally_body.get());
        return;
    }
    case ast::NodeKind::ThrowStmt:
        rename_in_expr(static_cast<ast::ThrowStmt *>(s)->value.get());
        return;
    case ast::NodeKind::SynchronizedStmt: {
        auto *y = static_cast<ast::SynchronizedStmt *>(s);
        rename_in_expr(y->target.get());
        rename_in_stmt(y->body.get());
        return;
    }
    default: return;
    }
}

} // namespace

void rename_idents(
    ast::Node *n, const std::unordered_map<std::string, std::string> &renames) {
    if (!n || renames.empty()) return;
    set_g_renames(&renames);
    switch (n->kind) {
    case ast::NodeKind::FunctionDecl: {
        auto *f = static_cast<ast::FunctionDecl *>(n);
        rename_in_stmt(f->body.get());
        break;
    }
    case ast::NodeKind::StructDecl: {
        auto *sd = static_cast<ast::StructDecl *>(n);
        for (auto &m : sd->methods)
            if (m) rename_in_stmt(m->body.get());
        break;
    }
    case ast::NodeKind::ClassDecl: {
        auto *cd = static_cast<ast::ClassDecl *>(n);
        for (auto &m : cd->methods)
            if (m) rename_in_stmt(m->body.get());
        break;
    }
    /* Los metodos por defecto de un concepto son cuerpos como los de un
     * struct, y se inyectan en tipos de OTRO modulo: tienen que seguir
     * nombrando lo que nombraban en el suyo. */
    case ast::NodeKind::ConceptDecl: {
        auto *cd = static_cast<ast::ConceptDecl *>(n);
        for (auto &m : cd->methods)
            if (m) rename_in_stmt(m->body.get());
        break;
    }
    default: break;
    }
    set_g_renames(nullptr);
}

} // namespace vxgen
} // namespace vx
