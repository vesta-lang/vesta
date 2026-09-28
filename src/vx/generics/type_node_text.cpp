/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file type_node_text.cpp
 * @brief Un tipo TAL Y COMO SE ESCRIBIO, en texto.
 *
 * Aparte de la deduccion (`generic_infer.cpp`) porque es AST puro y lo usa el
 * clonado, que tambien lo es: en el mismo objeto que la deduccion, pedirlo
 * arrastraba al enlace el comprobador entero -- y con el, el JIT --, y un
 * programa que solo clonaba arboles dejaba de enlazar.
 */

#include "vx/generics/generic_infer.h"

#include "vx/types.h"

#include <string>

namespace vx {
namespace generics {

std::string type_node_text(const ast::TypeNode *t) {
    if (t == nullptr) return std::string();
    switch (t->kind) {
    case ast::NodeKind::NamedTypeNode: {
        const auto *n = static_cast<const ast::NamedTypeNode *>(t);
        if (n->type_args.empty()) return n->name;
        std::string s = n->name + "<";
        for (size_t i = 0; i < n->type_args.size(); ++i) {
            if (i != 0) s += ", ";
            s += type_node_text(n->type_args[i].get());
        }
        return s + ">";
    }
    case ast::NodeKind::PrimitiveTypeNode: {
        const auto *p = static_cast<const ast::PrimitiveTypeNode *>(t);
        std::string s = primitive_name(p->prim);
        if (p->type_args.empty()) return s;
        s += "<";
        for (size_t i = 0; i < p->type_args.size(); ++i) {
            if (i != 0) s += ", ";
            s += type_node_text(p->type_args[i].get());
        }
        return s + ">";
    }
    case ast::NodeKind::PointerTypeNode:
        return type_node_text(static_cast<const ast::PointerTypeNode *>(t)
                                  ->pointee.get()) +
               "*";
    case ast::NodeKind::ArrayTypeNode:
        return type_node_text(static_cast<const ast::ArrayTypeNode *>(t)
                                  ->element_type.get()) +
               "[]";
    case ast::NodeKind::FunctionTypeNode: {
        const auto *f = static_cast<const ast::FunctionTypeNode *>(t);
        std::string s = "fn(";
        for (size_t i = 0; i < f->param_types.size(); ++i) {
            if (i != 0) s += ", ";
            s += type_node_text(f->param_types[i].get());
        }
        s += ")";
        if (f->return_type) s += " -> " + type_node_text(f->return_type.get());
        return s;
    }
    default: return std::string();
    }
}

} // namespace generics
} // namespace vx
