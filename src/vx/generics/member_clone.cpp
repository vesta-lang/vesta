/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file member_clone.cpp
 * @brief La UNICA lista de "que se copia" de un metodo o de un campo.
 *
 * El porque, en @c vx/generics/member_clone.h.
 */

#include "vx/generics/member_clone.h"

namespace vx {
namespace vxgen {
namespace {

/**
 * @brief Copia un bloque clonado como bloque (el clon de una sentencia
 *        devuelve la base).
 * @param b El bloque original, o @c nullptr.
 * @param g La sustitucion.
 * @return La copia, o @c nullptr.
 */
std::unique_ptr<ast::BlockStmt> clone_block(const ast::BlockStmt *b,
                                            const GenSubst &g) {
    if (b == nullptr) return nullptr;
    auto cb = clone_stmt(b, g);
    if (!cb || cb->kind != ast::NodeKind::BlockStmt) return nullptr;
    return std::unique_ptr<ast::BlockStmt>(
        static_cast<ast::BlockStmt *>(cb.release()));
}

} // namespace

std::unique_ptr<ast::ClassMethodDecl>
clone_method_with_subst(const ast::ClassMethodDecl &m, const GenSubst &g,
                        MethodBodyCopy body) {
    auto nm = std::make_unique<ast::ClassMethodDecl>();
    nm->loc = m.loc;
    nm->name = m.name;
    nm->visibility = m.visibility;
    nm->is_static = m.is_static;
    nm->is_final = m.is_final;
    nm->is_override = m.is_override;
    nm->origin = m.origin;
    nm->injection = m.injection;
    nm->is_virtual = m.is_virtual;
    // Un constructor `comptime` que se hereda o se instancia sigue siendolo, y
    // sigue siendo un CONSTRUCTOR: sin las dos marcas el clon se trataba como
    // codigo normal.
    nm->is_constructor = m.is_constructor;
    nm->is_comptime = m.is_comptime;
    nm->is_destructor = m.is_destructor;
    nm->advice_kind = m.advice_kind;
    nm->advice_target = m.advice_target;
    // Contratos de efectos y coste: son del METODO y viajan a cada copia.  Sin
    // copiarlos se evaporaban en silencio y no se verificaban en ninguna.
    nm->contract_pure = m.contract_pure;
    nm->contract_nothrow = m.contract_nothrow;
    nm->contract_nopanic = m.contract_nopanic;
    nm->contract_alloc = m.contract_alloc;
    nm->contract_alloc_partial = m.contract_alloc_partial;
    nm->contract_stack = m.contract_stack;
    nm->contract_stack_partial = m.contract_stack_partial;
    nm->complexity_expr = m.complexity_expr;
    nm->complexity_vars = m.complexity_vars;
    nm->complexity_partial_pre = m.complexity_partial_pre;
    nm->complexity_partial_post = m.complexity_partial_post;
    nm->complexity_total_pre = m.complexity_total_pre;
    nm->complexity_total_post = m.complexity_total_post;
    nm->complexity_pending = m.complexity_pending;
    nm->footprint_pending = m.footprint_pending;
    nm->property_kind = m.property_kind;
    nm->property_name = m.property_name;
    nm->is_inline = m.is_inline;
    // Los parametros de tipo del METODO (`mezcla<U>`) no los toca @p g: el
    // metodo sigue siendo generico y se instancia en cada llamada.
    nm->method_type_params = m.method_type_params;
    nm->type_bounds = m.type_bounds;
    if (m.return_type)
        nm->return_type = clone_type_with_subst(m.return_type.get(), g);
    nm->params.reserve(m.params.size());
    for (const auto &p : m.params)
        if (p) nm->params.push_back(clone_param_with_subst(*p, g));
    if (body == MethodBodyCopy::Clone) nm->body = clone_block(m.body.get(), g);
    return nm;
}

ast::StructFieldDecl clone_struct_field_with_subst(const ast::StructFieldDecl &f,
                                                   const GenSubst &g) {
    ast::StructFieldDecl nf;
    nf.loc = f.loc;
    nf.name = f.name;
    nf.type = clone_type_with_subst(f.type.get(), g);
    nf.dir = f.dir;
    nf.is_static = f.is_static;
    // Un miembro ANONIMO -- union/struct sin nombre -- o un array/overlay se
    // copiaba como campo escalar sin nombre y se perdian sus subcampos (la
    // union lo64/hi64/bytes de un entero ancho).
    nf.is_anonymous = f.is_anonymous;
    nf.bit_width = f.bit_width;
    nf.explicit_offset = f.explicit_offset;
    nf.offset_expr = clone_expr(f.offset_expr.get(), g);
    nf.offset_block = clone_block(f.offset_block.get(), g);
    nf.array_count = clone_expr(f.array_count.get(), g);
    nf.array_stride = clone_expr(f.array_stride.get(), g);
    nf.element_block = clone_block(f.element_block.get(), g);
    nf.is_array = f.is_array;
    nf.overlaps_with = f.overlaps_with;
    nf.overlaps_locs = f.overlaps_locs;
    nf.endian = f.endian;
    nf.endian_expr = clone_expr(f.endian_expr.get(), g);
    nf.default_init = clone_expr(f.default_init.get(), g);
    nf.is_comptime = f.is_comptime;
    nf.origin = f.origin;
    nf.injection = f.injection;
    nf.visibility = f.visibility;
    return nf;
}

ast::ClassFieldDecl clone_class_field_with_subst(const ast::ClassFieldDecl &f,
                                                 const GenSubst &g) {
    ast::ClassFieldDecl nf;
    nf.loc = f.loc;
    nf.name = f.name;
    nf.type = clone_type_with_subst(f.type.get(), g);
    nf.init = clone_expr(f.init.get(), g);
    nf.dir = f.dir;
    nf.visibility = f.visibility;
    nf.is_static = f.is_static;
    nf.is_final = f.is_final;
    nf.lombok_getter = f.lombok_getter;
    nf.lombok_setter = f.lombok_setter;
    nf.lombok_nonnull = f.lombok_nonnull;
    nf.lombok_with = f.lombok_with;
    nf.lombok_getter_lazy = f.lombok_getter_lazy;
    nf.origin = f.origin;
    return nf;
}

} // namespace vxgen
} // namespace vx
