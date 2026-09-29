/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file fields_decls.h
 * @brief Los campos de las declaraciones de FUNCION y de sus piezas: funcion
 *        libre, metodo, campo de struct y de clase, cotas, procedencia y
 *        contratos pendientes.
 *
 * Solo lo incluye `vx/ast/fields.h`.  Los tipos y el resto de declaraciones de
 * nivel superior, en `fields_type_decls.h`.  Un campo nuevo en una
 * declaracion va tambien a su lista, con su papel.
 *
 * Criterio de papel: @c parsed es lo que el programador ESCRIBIO -- aunque una
 * fase posterior lo complete o lo reparta sin inventar nada (`@Data` que
 * enciende `@Getter`, la cabecera `<...>` que se clasifica, el `@complexity`
 * con `when:` que se resuelve) --; @c annotated es lo que el compilador
 * DEDUCE y se recalcula en cada copia (la instancia de donde viene, la
 * etiqueta de una sobrecarga, el hueco en un layout, el fichero al que llevo
 * un import).  Los casos dudosos llevan su porque junto al campo.
 */

#ifndef VX_AST_FIELDS_DECLS_H
#define VX_AST_FIELDS_DECLS_H

namespace vx::ast::fields {

/// Una cota `<T: A + B<x>>`.
template <> struct Of<TypeBound> {
    static constexpr auto list() {
        return std::make_tuple(parsed(&TypeBound::type_param, "type_param"),
                               parsed(&TypeBound::concepts, "concepts"),
                               parsed(&TypeBound::loc, "loc"));
    }
};

/**
 * De donde viene un miembro.  El parser lo deja en @c Written y lo reescribe
 * quien COPIA el miembro a otro tipo (aplanado de la herencia, conceptos).
 * Es parte de lo que el miembro ES -- un campo heredado sigue siendolo en
 * cualquier copia de su tipo --, no algo que se recalcule sobre la copia, asi
 * que viaja como lo escrito.
 */
template <> struct Of<MemberOrigin> {
    static constexpr auto list() {
        return std::make_tuple(parsed(&MemberOrigin::kind, "kind"),
                               parsed(&MemberOrigin::via, "via"),
                               parsed(&MemberOrigin::original, "original"));
    }
};

/// Un `@complexity(..., when: ...)` sin resolver: lo escribe el parser.
template <> struct Of<PendingComplexity> {
    static constexpr auto list() {
        return std::make_tuple(
            parsed(&PendingComplexity::when, "when"),
            parsed(&PendingComplexity::expr, "expr"),
            parsed(&PendingComplexity::vars, "vars"),
            parsed(&PendingComplexity::partial_pre, "partial_pre"),
            parsed(&PendingComplexity::partial_post, "partial_post"),
            parsed(&PendingComplexity::total_pre, "total_pre"),
            parsed(&PendingComplexity::total_post, "total_post"));
    }
};

/// Un contrato de huella con `when:` sin resolver: lo escribe el parser.
template <> struct Of<PendingFootprint> {
    static constexpr auto list() {
        return std::make_tuple(
            parsed(&PendingFootprint::when, "when"),
            parsed(&PendingFootprint::pure, "pure"),
            parsed(&PendingFootprint::nothrow_, "nothrow_"),
            parsed(&PendingFootprint::nopanic, "nopanic"),
            parsed(&PendingFootprint::alloc, "alloc"),
            parsed(&PendingFootprint::alloc_partial, "alloc_partial"),
            parsed(&PendingFootprint::stack, "stack"),
            parsed(&PendingFootprint::stack_partial, "stack_partial"));
    }
};

/**
 * Una funcion libre.
 *
 * - `type_params`, `is_specialization`, `spec_pattern` y
 *   `generic_head_unresolved` son UNA pieza: el parser deja lo escrito entre
 *   `<...>` en `spec_pattern` con la marca, y `classify_generic_heads` lo
 *   REPARTE entre los tres sin anadir nada.  Antes y despues es lo escrito,
 *   en dos formas; marcar alguno como anotado daria una copia con media
 *   cabecera.  Las cuatro, del parser.
 * - Los `complexity_*` y `contract_*` los escribe el parser; los que llevan
 *   `when:` quedan en `*_pending` y se VUELCAN sobre esos mismos campos al
 *   resolverse, vaciando la lista.  La pareja es siempre coherente -- o sin
 *   resolver o resuelta --, asi que las dos viajan juntas como lo escrito.
 * - `is_imported_comptime` lo pone quien importa, no el parser: dice que ESTA
 *   copia no se baja.  Una copia hecha aqui (una instancia) si se baja.
 */
template <> struct Of<FunctionDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(
                parsed(&FunctionDecl::return_type, "return_type"),
                parsed(&FunctionDecl::name, "name"),
                parsed(&FunctionDecl::params, "params"),
                parsed(&FunctionDecl::body, "body"),
                parsed(&FunctionDecl::type_bounds, "type_bounds"),
                annotated(&FunctionDecl::instance_of, "instance_of"),
                annotated(&FunctionDecl::instance_template,
                          "instance_template"),
                annotated(&FunctionDecl::instance_site, "instance_site"),
                annotated(&FunctionDecl::instance_bindings,
                          "instance_bindings"),
                annotated(&FunctionDecl::instance_parent, "instance_parent"),
                parsed(&FunctionDecl::is_specialization, "is_specialization"),
                parsed(&FunctionDecl::spec_pattern, "spec_pattern"),
                parsed(&FunctionDecl::generic_head_unresolved,
                       "generic_head_unresolved"),
                parsed(&FunctionDecl::is_public, "is_public"),
                parsed(&FunctionDecl::is_internal, "is_internal"),
                parsed(&FunctionDecl::is_async, "is_async"),
                parsed(&FunctionDecl::is_comptime, "is_comptime"),
                parsed(&FunctionDecl::type_params, "type_params"),
                parsed(&FunctionDecl::is_macro, "is_macro"),
                parsed(&FunctionDecl::is_pure, "is_pure"),
                annotated(&FunctionDecl::is_imported_comptime,
                          "is_imported_comptime"),
                parsed(&FunctionDecl::is_forward_decl, "is_forward_decl"),
                annotated(&FunctionDecl::mangled_label, "mangled_label"),
                parsed(&FunctionDecl::fp_contract, "fp_contract"),
                parsed(&FunctionDecl::attr_section, "attr_section"),
                parsed(&FunctionDecl::attr_section_perms,
                       "attr_section_perms"),
                parsed(&FunctionDecl::attr_at, "attr_at"),
                parsed(&FunctionDecl::attr_order, "attr_order"),
                parsed(&FunctionDecl::provides_builtin, "provides_builtin"),
                parsed(&FunctionDecl::is_naked, "is_naked"),
                parsed(&FunctionDecl::is_inline, "is_inline"),
                parsed(&FunctionDecl::is_no_idiom, "is_no_idiom"),
                parsed(&FunctionDecl::mode_variant_jit, "mode_variant_jit"),
                parsed(&FunctionDecl::is_noexcept, "is_noexcept"),
                parsed(&FunctionDecl::is_string_concat_override,
                       "is_string_concat_override"),
                parsed(&FunctionDecl::is_string_eq_override,
                       "is_string_eq_override"),
                parsed(&FunctionDecl::is_sync_impl, "is_sync_impl"),
                parsed(&FunctionDecl::helper_override_target,
                       "helper_override_target"),
                parsed(&FunctionDecl::hook_point, "hook_point"),
                parsed(&FunctionDecl::hook_selector, "hook_selector"),
                parsed(&FunctionDecl::is_no_instrument, "is_no_instrument"),
                parsed(&FunctionDecl::complexity_expr, "complexity_expr"),
                parsed(&FunctionDecl::complexity_vars, "complexity_vars"),
                parsed(&FunctionDecl::complexity_partial_pre,
                       "complexity_partial_pre"),
                parsed(&FunctionDecl::complexity_partial_post,
                       "complexity_partial_post"),
                parsed(&FunctionDecl::complexity_total_pre,
                       "complexity_total_pre"),
                parsed(&FunctionDecl::complexity_total_post,
                       "complexity_total_post"),
                parsed(&FunctionDecl::complexity_pending,
                       "complexity_pending"),
                parsed(&FunctionDecl::footprint_pending, "footprint_pending"),
                parsed(&FunctionDecl::contract_pure, "contract_pure"),
                parsed(&FunctionDecl::contract_nothrow, "contract_nothrow"),
                parsed(&FunctionDecl::contract_nopanic, "contract_nopanic"),
                parsed(&FunctionDecl::contract_alloc, "contract_alloc"),
                parsed(&FunctionDecl::contract_alloc_partial,
                       "contract_alloc_partial"),
                parsed(&FunctionDecl::contract_stack, "contract_stack"),
                parsed(&FunctionDecl::contract_stack_partial,
                       "contract_stack_partial")));
    }
};

/**
 * Un metodo o constructor.  `layout_slot` lo anota el montaje del layout: es
 * el hueco de ESTE tipo, y una copia a otro tipo tiene el suyo.  Los
 * contratos y el `@complexity`, como en @ref Of<FunctionDecl>.
 */
template <> struct Of<ClassMethodDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(
                parsed(&ClassMethodDecl::return_type, "return_type"),
                parsed(&ClassMethodDecl::name, "name"),
                parsed(&ClassMethodDecl::params, "params"),
                parsed(&ClassMethodDecl::body, "body"),
                parsed(&ClassMethodDecl::visibility, "visibility"),
                annotated(&ClassMethodDecl::layout_slot, "layout_slot"),
                parsed(&ClassMethodDecl::is_static, "is_static"),
                parsed(&ClassMethodDecl::is_final, "is_final"),
                parsed(&ClassMethodDecl::is_override, "is_override"),
                parsed(&ClassMethodDecl::origin, "origin"),
                parsed(&ClassMethodDecl::injection, "injection"),
                parsed(&ClassMethodDecl::is_virtual, "is_virtual"),
                parsed(&ClassMethodDecl::is_constructor, "is_constructor"),
                parsed(&ClassMethodDecl::is_comptime, "is_comptime"),
                parsed(&ClassMethodDecl::is_destructor, "is_destructor"),
                parsed(&ClassMethodDecl::advice_kind, "advice_kind"),
                parsed(&ClassMethodDecl::advice_target, "advice_target"),
                parsed(&ClassMethodDecl::contract_pure, "contract_pure"),
                parsed(&ClassMethodDecl::contract_nothrow, "contract_nothrow"),
                parsed(&ClassMethodDecl::contract_nopanic, "contract_nopanic"),
                parsed(&ClassMethodDecl::contract_alloc, "contract_alloc"),
                parsed(&ClassMethodDecl::contract_alloc_partial,
                       "contract_alloc_partial"),
                parsed(&ClassMethodDecl::contract_stack, "contract_stack"),
                parsed(&ClassMethodDecl::contract_stack_partial,
                       "contract_stack_partial"),
                parsed(&ClassMethodDecl::complexity_expr, "complexity_expr"),
                parsed(&ClassMethodDecl::complexity_vars, "complexity_vars"),
                parsed(&ClassMethodDecl::complexity_partial_pre,
                       "complexity_partial_pre"),
                parsed(&ClassMethodDecl::complexity_partial_post,
                       "complexity_partial_post"),
                parsed(&ClassMethodDecl::complexity_total_pre,
                       "complexity_total_pre"),
                parsed(&ClassMethodDecl::complexity_total_post,
                       "complexity_total_post"),
                parsed(&ClassMethodDecl::complexity_pending,
                       "complexity_pending"),
                parsed(&ClassMethodDecl::footprint_pending,
                       "footprint_pending"),
                parsed(&ClassMethodDecl::property_kind, "property_kind"),
                parsed(&ClassMethodDecl::property_name, "property_name"),
                parsed(&ClassMethodDecl::is_inline, "is_inline"),
                parsed(&ClassMethodDecl::method_type_params,
                       "method_type_params"),
                parsed(&ClassMethodDecl::type_bounds, "type_bounds")));
    }
};

/// Un campo de struct (o de concepto).
template <> struct Of<StructFieldDecl> {
    static constexpr auto list() {
        return std::make_tuple(
            parsed(&StructFieldDecl::type, "type"),
            parsed(&StructFieldDecl::name, "name"),
            parsed(&StructFieldDecl::loc, "loc"),
            parsed(&StructFieldDecl::dir, "dir"),
            parsed(&StructFieldDecl::is_static, "is_static"),
            parsed(&StructFieldDecl::is_anonymous, "is_anonymous"),
            parsed(&StructFieldDecl::bit_width, "bit_width"),
            parsed(&StructFieldDecl::explicit_offset, "explicit_offset"),
            parsed(&StructFieldDecl::offset_expr, "offset_expr"),
            parsed(&StructFieldDecl::offset_block, "offset_block"),
            parsed(&StructFieldDecl::array_count, "array_count"),
            parsed(&StructFieldDecl::array_stride, "array_stride"),
            parsed(&StructFieldDecl::element_block, "element_block"),
            parsed(&StructFieldDecl::is_array, "is_array"),
            parsed(&StructFieldDecl::overlaps_with, "overlaps_with"),
            parsed(&StructFieldDecl::overlaps_locs, "overlaps_locs"),
            parsed(&StructFieldDecl::endian, "endian"),
            parsed(&StructFieldDecl::endian_expr, "endian_expr"),
            parsed(&StructFieldDecl::default_init, "default_init"),
            parsed(&StructFieldDecl::is_comptime, "is_comptime"),
            parsed(&StructFieldDecl::origin, "origin"),
            parsed(&StructFieldDecl::injection, "injection"),
            parsed(&StructFieldDecl::visibility, "visibility"));
    }
};

/**
 * Un campo de clase.  `is_final` y el `is_nonnull` de su tipo los puede
 * encender `@Value`/`@NonNull` al expandir Lombok: completan lo escrito, del
 * parser.
 */
template <> struct Of<ClassFieldDecl> {
    static constexpr auto list() {
        return std::make_tuple(
            parsed(&ClassFieldDecl::type, "type"),
            parsed(&ClassFieldDecl::name, "name"),
            parsed(&ClassFieldDecl::init, "init"),
            parsed(&ClassFieldDecl::loc, "loc"),
            parsed(&ClassFieldDecl::dir, "dir"),
            parsed(&ClassFieldDecl::visibility, "visibility"),
            parsed(&ClassFieldDecl::is_static, "is_static"),
            parsed(&ClassFieldDecl::is_final, "is_final"),
            parsed(&ClassFieldDecl::lombok_getter, "lombok_getter"),
            parsed(&ClassFieldDecl::lombok_setter, "lombok_setter"),
            parsed(&ClassFieldDecl::lombok_nonnull, "lombok_nonnull"),
            parsed(&ClassFieldDecl::lombok_with, "lombok_with"),
            parsed(&ClassFieldDecl::lombok_getter_lazy, "lombok_getter_lazy"),
            parsed(&ClassFieldDecl::origin, "origin"));
    }
};

} // namespace vx::ast::fields

#endif // VX_AST_FIELDS_DECLS_H
