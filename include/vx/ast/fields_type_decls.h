/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file fields_type_decls.h
 * @brief Los campos de las declaraciones de TIPO (struct, enum, clase,
 *        alias) y del resto de las de nivel superior (globales, conceptos,
 *        `extension`/`impl`, externas, `bytes`, `import`, namespaces).
 *
 * Solo lo incluye `vx/ast/fields.h`, despues de `fields_decls.h` (funciones,
 * metodos y campos, que estas usan).  El criterio de papel es el de
 * `fields_decls.h`.  Un campo nuevo en una de estas declaraciones va tambien
 * a su lista.
 */

#ifndef VX_AST_FIELDS_TYPE_DECLS_H
#define VX_AST_FIELDS_TYPE_DECLS_H

namespace vx::ast::fields {

/// Un struct.  La cabecera generica, como en @ref Of<FunctionDecl>.
template <> struct Of<StructDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(
                parsed(&StructDecl::name, "name"),
                parsed(&StructDecl::super_name, "super_name"),
                parsed(&StructDecl::super_args, "super_args"),
                parsed(&StructDecl::interface_names, "interface_names"),
                parsed(&StructDecl::fields, "fields"),
                parsed(&StructDecl::methods, "methods"),
                parsed(&StructDecl::is_public, "is_public"),
                parsed(&StructDecl::is_introspect, "is_introspect"),
                parsed(&StructDecl::is_overlay, "is_overlay"),
                parsed(&StructDecl::is_union, "is_union"),
                parsed(&StructDecl::is_abstract, "is_abstract"),
                parsed(&StructDecl::type_params, "type_params"),
                parsed(&StructDecl::type_bounds, "type_bounds"),
                parsed(&StructDecl::is_specialization, "is_specialization"),
                parsed(&StructDecl::spec_pattern, "spec_pattern"),
                parsed(&StructDecl::generic_head_unresolved,
                       "generic_head_unresolved"),
                parsed(&StructDecl::contract_pod, "contract_pod"),
                parsed(&StructDecl::contract_no_heap, "contract_no_heap"),
                parsed(&StructDecl::contract_size, "contract_size"),
                parsed(&StructDecl::attr_align, "attr_align"),
                parsed(&StructDecl::is_incomplete, "is_incomplete")));
    }
};

/// Una variante de enum.
template <> struct Of<EnumVariantDecl> {
    static constexpr auto list() {
        return std::make_tuple(
            parsed(&EnumVariantDecl::name, "name"),
            parsed(&EnumVariantDecl::value_expr, "value_expr"),
            parsed(&EnumVariantDecl::field_types, "field_types"),
            parsed(&EnumVariantDecl::loc, "loc"));
    }
};

/**
 * Un enum.  `backing_type` lo escribe el parser (`: u8`); en un enum C sin
 * tipo escrito el comprobador lo INFIERE del rango de valores y lo apunta
 * aqui.  Es la misma respuesta para cualquier copia -- sale de los valores
 * escritos --, asi que viaja como lo escrito.
 */
template <> struct Of<EnumDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(
                parsed(&EnumDecl::name, "name"),
                parsed(&EnumDecl::backing_type, "backing_type"),
                parsed(&EnumDecl::c_style_auto_backing,
                       "c_style_auto_backing"),
                parsed(&EnumDecl::variants, "variants"),
                parsed(&EnumDecl::is_introspect, "is_introspect"),
                parsed(&EnumDecl::is_public, "is_public"),
                parsed(&EnumDecl::type_params, "type_params"),
                parsed(&EnumDecl::type_bounds, "type_bounds"),
                parsed(&EnumDecl::contract_pod, "contract_pod"),
                parsed(&EnumDecl::contract_no_heap, "contract_no_heap"),
                parsed(&EnumDecl::contract_size, "contract_size")));
    }
};

/**
 * Una clase.  Los `lombok_*` de clase los puede encender la expansion de
 * `@Data`/`@Value` (combos de otros): completan lo escrito, del parser.
 */
template <> struct Of<ClassDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(
                parsed(&ClassDecl::name, "name"),
                parsed(&ClassDecl::super_name, "super_name"),
                parsed(&ClassDecl::super_args, "super_args"),
                parsed(&ClassDecl::interface_names, "interface_names"),
                parsed(&ClassDecl::fields, "fields"),
                parsed(&ClassDecl::methods, "methods"),
                parsed(&ClassDecl::is_final, "is_final"),
                parsed(&ClassDecl::is_public, "is_public"),
                parsed(&ClassDecl::type_params, "type_params"),
                parsed(&ClassDecl::type_bounds, "type_bounds"),
                parsed(&ClassDecl::is_specialization, "is_specialization"),
                parsed(&ClassDecl::spec_pattern, "spec_pattern"),
                parsed(&ClassDecl::generic_head_unresolved,
                       "generic_head_unresolved"),
                parsed(&ClassDecl::is_aspect, "is_aspect"),
                parsed(&ClassDecl::is_interface, "is_interface"),
                parsed(&ClassDecl::is_introspect, "is_introspect"),
                parsed(&ClassDecl::contract_pod, "contract_pod"),
                parsed(&ClassDecl::contract_no_heap, "contract_no_heap"),
                parsed(&ClassDecl::contract_size, "contract_size"),
                parsed(&ClassDecl::lombok_getter, "lombok_getter"),
                parsed(&ClassDecl::lombok_setter, "lombok_setter"),
                parsed(&ClassDecl::lombok_tostring, "lombok_tostring"),
                parsed(&ClassDecl::lombok_equals_hash, "lombok_equals_hash"),
                parsed(&ClassDecl::lombok_no_args_ctor, "lombok_no_args_ctor"),
                parsed(&ClassDecl::lombok_all_args_ctor,
                       "lombok_all_args_ctor"),
                parsed(&ClassDecl::lombok_required_ctor,
                       "lombok_required_ctor"),
                parsed(&ClassDecl::lombok_data, "lombok_data"),
                parsed(&ClassDecl::lombok_value, "lombok_value"),
                parsed(&ClassDecl::lombok_builder, "lombok_builder"),
                parsed(&ClassDecl::lombok_with_all, "lombok_with_all"),
                parsed(&ClassDecl::lombok_log, "lombok_log"),
                parsed(&ClassDecl::lombok_sync_methods,
                       "lombok_sync_methods")));
    }
};

/// Una variable de modulo.
template <> struct Of<GlobalVarDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(
                parsed(&GlobalVarDecl::type, "type"),
                parsed(&GlobalVarDecl::name, "name"),
                parsed(&GlobalVarDecl::init, "init"),
                parsed(&GlobalVarDecl::is_const, "is_const"),
                parsed(&GlobalVarDecl::is_public, "is_public"),
                parsed(&GlobalVarDecl::is_internal, "is_internal"),
                parsed(&GlobalVarDecl::is_comptime, "is_comptime"),
                parsed(&GlobalVarDecl::is_thread_local, "is_thread_local"),
                parsed(&GlobalVarDecl::attr_hot, "attr_hot"),
                parsed(&GlobalVarDecl::attr_cold, "attr_cold"),
                parsed(&GlobalVarDecl::attr_align, "attr_align"),
                parsed(&GlobalVarDecl::attr_section, "attr_section"),
                parsed(&GlobalVarDecl::attr_section_perms,
                       "attr_section_perms")));
    }
};

/// Un concepto (las tres formas).
template <> struct Of<ConceptDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(parsed(&ConceptDecl::name, "name"),
                            parsed(&ConceptDecl::type_params, "type_params"),
                            parsed(&ConceptDecl::ckind, "ckind"),
                            parsed(&ConceptDecl::predicate, "predicate"),
                            parsed(&ConceptDecl::body, "body"),
                            parsed(&ConceptDecl::methods, "methods"),
                            parsed(&ConceptDecl::fields, "fields"),
                            parsed(&ConceptDecl::is_public, "is_public")));
    }
};

/// `extension Tipo { metodos }`.
template <> struct Of<ExtensionDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(parsed(&ExtensionDecl::target_type, "target_type"),
                            parsed(&ExtensionDecl::methods, "methods"),
                            parsed(&ExtensionDecl::is_public, "is_public")));
    }
};

/// `impl Concepto<args> for Tipo { metodos }`.
template <> struct Of<ImplDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(parsed(&ImplDecl::concept_name, "concept_name"),
                            parsed(&ImplDecl::concept_args, "concept_args"),
                            parsed(&ImplDecl::target_type, "target_type"),
                            parsed(&ImplDecl::methods, "methods"),
                            parsed(&ImplDecl::is_public, "is_public")));
    }
};

/// Lo declarado de una funcion externa: el parser ya lo deja resuelto contra
/// el objetivo activo.
template <> struct Of<ExternEffects> {
    static constexpr auto list() {
        return std::make_tuple(
            parsed(&ExternEffects::any, "any"),
            parsed(&ExternEffects::io, "io"),
            parsed(&ExternEffects::may_throw, "may_throw"),
            parsed(&ExternEffects::may_panic, "may_panic"),
            parsed(&ExternEffects::allocates, "allocates"),
            parsed(&ExternEffects::reads_global, "reads_global"),
            parsed(&ExternEffects::writes_global, "writes_global"),
            parsed(&ExternEffects::nondeterministic, "nondeterministic"),
            parsed(&ExternEffects::may_block, "may_block"),
            parsed(&ExternEffects::may_trap, "may_trap"),
            parsed(&ExternEffects::throw_origin, "throw_origin"),
            parsed(&ExternEffects::panic_origin, "panic_origin"),
            parsed(&ExternEffects::trap_kinds, "trap_kinds"),
            parsed(&ExternEffects::traps_sin_acotar, "traps_sin_acotar"),
            parsed(&ExternEffects::reads_world, "reads_world"),
            parsed(&ExternEffects::writes_world, "writes_world"),
            parsed(&ExternEffects::reads_env_visto, "reads_env_visto"),
            parsed(&ExternEffects::writes_env_visto, "writes_env_visto"),
            parsed(&ExternEffects::returns_fresh, "returns_fresh"),
            parsed(&ExternEffects::frees_pointee, "frees_pointee"));
    }
};

/// `extern "lib" { fn ...; }`, una por funcion.
template <> struct Of<ExternFnDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(parsed(&ExternFnDecl::lib, "lib"),
                            parsed(&ExternFnDecl::return_type, "return_type"),
                            parsed(&ExternFnDecl::name, "name"),
                            parsed(&ExternFnDecl::params, "params"),
                            parsed(&ExternFnDecl::effects, "effects")));
    }
};

/// Una referencia a un simbolo dentro de un bloque `bytes`.
template <> struct Of<BytesSymRef> {
    static constexpr auto list() {
        return std::make_tuple(parsed(&BytesSymRef::offset, "offset"),
                               parsed(&BytesSymRef::sym, "sym"),
                               parsed(&BytesSymRef::width, "width"),
                               parsed(&BytesSymRef::is_rel, "is_rel"));
    }
};

/// `bytes nombre { db ...; }`: los bytes ya los resuelve el parser.
template <> struct Of<BytesDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(
                parsed(&BytesDecl::name, "name"),
                parsed(&BytesDecl::attr_section, "attr_section"),
                parsed(&BytesDecl::attr_section_perms, "attr_section_perms"),
                parsed(&BytesDecl::attr_at, "attr_at"),
                parsed(&BytesDecl::attr_order, "attr_order"),
                parsed(&BytesDecl::is_public, "is_public"),
                parsed(&BytesDecl::data, "data"),
                parsed(&BytesDecl::sym_refs, "sym_refs"),
                parsed(&BytesDecl::is_asm, "is_asm"),
                parsed(&BytesDecl::asm_body, "asm_body"),
                parsed(&BytesDecl::asm_bits, "asm_bits")));
    }
};

/// Una conversion declarada en un `typedef ... new { explicit from T; }`.
template <> struct Of<TypeAliasDecl::ExplicitConv> {
    static constexpr auto list() {
        return std::make_tuple(
            parsed(&TypeAliasDecl::ExplicitConv::type, "type"),
            parsed(&TypeAliasDecl::ExplicitConv::is_public, "is_public"));
    }
};

/// `typedef` / `using`.
template <> struct Of<TypeAliasDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(
                parsed(&TypeAliasDecl::name, "name"),
                parsed(&TypeAliasDecl::aliased, "aliased"),
                parsed(&TypeAliasDecl::is_using_form, "is_using_form"),
                parsed(&TypeAliasDecl::is_public, "is_public"),
                parsed(&TypeAliasDecl::is_newtype, "is_newtype"),
                parsed(&TypeAliasDecl::is_opaque, "is_opaque"),
                parsed(&TypeAliasDecl::align_override, "align_override"),
                parsed(&TypeAliasDecl::explicit_from, "explicit_from"),
                parsed(&TypeAliasDecl::explicit_to, "explicit_to"),
                parsed(&TypeAliasDecl::implicit_from, "implicit_from"),
                parsed(&TypeAliasDecl::implicit_to, "implicit_to")));
    }
};

/// Un simbolo de `import ... only A as B`.
template <> struct Of<ImportDecl::OnlySymbol> {
    static constexpr auto list() {
        return std::make_tuple(
            parsed(&ImportDecl::OnlySymbol::name, "name"),
            parsed(&ImportDecl::OnlySymbol::rename, "rename"));
    }
};

/// Un `import`.  `resolved_path` lo apunta el resolvedor de modulos.
template <> struct Of<ImportDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(
                parsed(&ImportDecl::path, "path"),
                parsed(&ImportDecl::by_namespace, "by_namespace"),
                annotated(&ImportDecl::resolved_path, "resolved_path"),
                parsed(&ImportDecl::alias, "alias"),
                parsed(&ImportDecl::only_symbols, "only_symbols"),
                parsed(&ImportDecl::only_all, "only_all"),
                parsed(&ImportDecl::is_public_reexport,
                       "is_public_reexport")));
    }
};

/// `namespace a.b { ... }` o `namespace a.b;`.
template <> struct Of<NamespaceDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(
                parsed(&NamespaceDecl::name, "name"),
                parsed(&NamespaceDecl::decls, "decls"),
                parsed(&NamespaceDecl::is_statement_form, "is_statement_form"),
                parsed(&NamespaceDecl::package_id_override,
                       "package_id_override")));
    }
};

} // namespace vx::ast::fields

#endif // VX_AST_FIELDS_TYPE_DECLS_H
