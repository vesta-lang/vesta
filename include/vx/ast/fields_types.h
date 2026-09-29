/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file fields_types.h
 * @brief Los campos de la base comun (`Node`), de los nodos de TIPO y de las
 *        piezas que los acompanan (`ParamDecl`, `ConceptRef`).
 *
 * Solo lo incluye `vx/ast/fields.h`.  Un campo nuevo en uno de estos nodos va
 * tambien a su lista.
 */

#ifndef VX_AST_FIELDS_TYPES_H
#define VX_AST_FIELDS_TYPES_H

namespace vx::ast::fields {

/// La base de todo nodo.  `kind` no esta: lo fija el constructor.
template <> struct Of<Node> {
    static constexpr auto list() {
        return std::make_tuple(parsed(&Node::loc, "loc"),
                               parsed(&Node::span_start, "span_start"),
                               parsed(&Node::span_end, "span_end"));
    }
};

/// Lo que todo tipo escrito lleva ademas de su forma.
template <> struct Of<TypeNode> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(parsed(&TypeNode::is_nonnull, "is_nonnull"),
                            parsed(&TypeNode::is_const, "is_const"),
                            parsed(&TypeNode::is_volatile, "is_volatile")));
    }
};

/// `i64`, `ArrayList<T>`.
template <> struct Of<PrimitiveTypeNode> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<TypeNode>::list(),
            std::make_tuple(parsed(&PrimitiveTypeNode::prim, "prim"),
                            parsed(&PrimitiveTypeNode::type_args, "type_args")));
    }
};

/// `Punto`, `Caja<T>`.
template <> struct Of<NamedTypeNode> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<TypeNode>::list(),
            std::make_tuple(parsed(&NamedTypeNode::name, "name"),
                            parsed(&NamedTypeNode::type_args, "type_args")));
    }
};

/// `T*`, `VirtualPtr<T>`.
template <> struct Of<PointerTypeNode> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<TypeNode>::list(),
            std::make_tuple(parsed(&PointerTypeNode::pointee, "pointee"),
                            parsed(&PointerTypeNode::is_virtual, "is_virtual")));
    }
};

/// `T[N]`, `T[]`.
template <> struct Of<ArrayTypeNode> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<TypeNode>::list(),
            std::make_tuple(
                parsed(&ArrayTypeNode::element_type, "element_type"),
                parsed(&ArrayTypeNode::size_expr, "size_expr")));
    }
};

/// `fn(T...) -> R`, `cfn(...)`.
template <> struct Of<FunctionTypeNode> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<TypeNode>::list(),
            std::make_tuple(
                parsed(&FunctionTypeNode::param_types, "param_types"),
                parsed(&FunctionTypeNode::return_type, "return_type"),
                parsed(&FunctionTypeNode::is_raw, "is_raw"),
                parsed(&FunctionTypeNode::is_variadic, "is_variadic"),
                parsed(&FunctionTypeNode::param_abi_regs, "param_abi_regs"),
                parsed(&FunctionTypeNode::param_dirs, "param_dirs")));
    }
};

/// Un tipo que sale de ejecutar una llamada al compilar.
template <> struct Of<ComputedTypeNode> {
    static constexpr auto list() {
        return std::tuple_cat(Of<TypeNode>::list(),
                              std::make_tuple(
                                  parsed(&ComputedTypeNode::expr, "expr")));
    }
};

/// Un parametro de funcion, de metodo o de lambda.
template <> struct Of<ParamDecl> {
    static constexpr auto list() {
        return std::tuple_cat(
            Of<Node>::list(),
            std::make_tuple(
                parsed(&ParamDecl::type, "type"),
                parsed(&ParamDecl::name, "name"),
                parsed(&ParamDecl::dir, "dir"),
                parsed(&ParamDecl::is_expr_capture, "is_expr_capture"),
                parsed(&ParamDecl::is_variadic, "is_variadic"),
                parsed(&ParamDecl::is_raw_variadic, "is_raw_variadic"),
                parsed(&ParamDecl::abi_reg, "abi_reg")));
    }
};

/// Un concepto exigido con sus argumentos (`View<i64>`).
template <> struct Of<ConceptRef> {
    static constexpr auto list() {
        return std::make_tuple(parsed(&ConceptRef::name, "name"),
                               parsed(&ConceptRef::args, "args"),
                               parsed(&ConceptRef::loc, "loc"));
    }
};

} // namespace vx::ast::fields

#endif // VX_AST_FIELDS_TYPES_H
