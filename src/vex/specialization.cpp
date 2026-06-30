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
 * @file specialization.cpp
 * @brief Especializacion total + parcial de structs genericos (#7).
 *
 * Tras el template PRIMARIO `struct Caja<T> { ... }`, el usuario puede
 * declarar especializaciones:
 *   - TOTAL:   `struct Caja<i64> { ... }`   (tipo concreto exacto)
 *   - PARCIAL: `struct Caja<T*> { ... }`    (patron; T es un param fresco)
 * Al instanciar `Caja<X>`, se elige la especializacion MAS ESPECIFICA que
 * matchee (exacta > patron > primario), y se clona ESA definicion.  Todo
 * compile-time; cero runtime.  Modulo separado de type_checker.cpp para
 * mantenerlo manejable.
 */

#include "vex/type_checker.h"

#include <unordered_set>

namespace vex {

namespace {

/// Matchea un type-node PATRON contra un @c Type concreto, ligando los params
/// frescos.  Un identificador en @p fresh liga al arg; uno concreto (o
/// primitivo / puntero / array) exige igualdad estructural.  Devuelve true si
/// matchea.
bool match_spec_pattern(TypeChecker &tc, const ast::TypeNode *pat,
                        const Type &arg,
                        const std::unordered_set<std::string> &fresh,
                        std::unordered_map<std::string, Type> &bindings) {
    if (!pat) return false;
    switch (pat->kind) {
    case ast::NodeKind::NamedTypeNode: {
        auto *n = static_cast<const ast::NamedTypeNode *>(pat);
        // Patron generico ANIDADO: `Inner<T>` (T fresco).  El arg debe ser
        // una instanciacion concreta de `Inner` (e.g. `Inner_i64`);
        // recuperamos sus type-args concretos via monomorph_info y los
        // matcheamos recursivamente contra los del patron.
        if (!n->type_args.empty()) {
            if (arg.kind != PrimitiveKind::STRUCT &&
                arg.kind != PrimitiveKind::CLASS)
                return false;
            const auto *mi = tc.monomorph_info(arg.struct_name);
            if (!mi || mi->template_name != n->name) return false;
            if (mi->type_arg_types.size() != n->type_args.size()) return false;
            for (size_t i = 0; i < n->type_args.size(); ++i) {
                if (!match_spec_pattern(tc, n->type_args[i].get(),
                                        mi->type_arg_types[i], fresh, bindings))
                    return false;
            }
            return true;
        }
        if (fresh.count(n->name)) {
            // Param fresco: liga al tipo concreto (parcial).  Si ya estaba
            // ligado, debe coincidir (consistencia de `Par<T, T>`).
            auto it = bindings.find(n->name);
            if (it != bindings.end()) return it->second == arg;
            bindings.emplace(n->name, arg);
            return true;
        }
        // Tipo concreto nombrado (especializacion total sobre un tipo de
        // usuario): igualdad exacta.
        const Type pt = tc.resolve_type_node(pat);
        return pt == arg;
    }
    case ast::NodeKind::PrimitiveTypeNode: {
        const Type pt = tc.resolve_type_node(pat);
        return pt == arg;
    }
    case ast::NodeKind::PointerTypeNode: {
        if (arg.kind != PrimitiveKind::PTR || !arg.pointee) return false;
        auto *p = static_cast<const ast::PointerTypeNode *>(pat);
        return match_spec_pattern(tc, p->pointee.get(), *arg.pointee, fresh,
                                  bindings);
    }
    case ast::NodeKind::ArrayTypeNode: {
        if (arg.kind != PrimitiveKind::ARRAY || !arg.pointee) return false;
        auto *a = static_cast<const ast::ArrayTypeNode *>(pat);
        return match_spec_pattern(tc, a->element_type.get(), *arg.pointee,
                                  fresh, bindings);
    }
    default:
        return false; // fn, etc.: no soportado como patron
    }
}

/// Nucleo generico de seleccion: dado un decl @c DeclT (struct/clase/funcion)
/// con @c is_specialization / @c spec_pattern / @c type_params, elige la
/// especializacion mas especifica que matchee @p args.  TOTAL (sin params
/// frescos) gana a PARCIAL; entre iguales, la primera declarada.
template <class DeclT>
const DeclT *select_spec_generic(TypeChecker &tc,
                                 const std::vector<size_t> &candidate_indices,
                                 const std::vector<std::unique_ptr<ast::Node>> &decls,
                                 const std::vector<Type> &args,
                                 std::vector<std::string> &out_params,
                                 std::vector<Type> &out_args) {
    const DeclT *best = nullptr;
    int best_score = -1;
    std::unordered_map<std::string, Type> best_bindings;

    for (size_t idx : candidate_indices) {
        if (idx >= decls.size()) continue;
        auto *spec = static_cast<const DeclT *>(decls[idx].get());
        if (!spec || !spec->is_specialization) continue;
        if (spec->spec_pattern.size() != args.size()) continue;

        std::unordered_set<std::string> fresh(spec->type_params.begin(),
                                              spec->type_params.end());
        std::unordered_map<std::string, Type> bindings;
        bool ok = true;
        for (size_t i = 0; i < args.size(); ++i) {
            if (!match_spec_pattern(tc, spec->spec_pattern[i].get(), args[i],
                                    fresh, bindings)) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        const int score = spec->type_params.empty() ? 100 : 50;
        if (score > best_score) {
            best_score = score;
            best = spec;
            best_bindings = bindings;
        }
    }

    if (!best) return nullptr;
    out_params = best->type_params;
    out_args.clear();
    out_args.reserve(out_params.size());
    for (const auto &p : out_params) {
        auto bit = best_bindings.find(p);
        out_args.push_back(bit != best_bindings.end() ? bit->second : Type{});
    }
    return best;
}

} // namespace

const ast::StructDecl *TypeChecker::select_struct_specialization(
    const std::string &base, const std::vector<Type> &args,
    std::vector<std::string> &out_params, std::vector<Type> &out_args) {
    auto it = struct_specializations_.find(base);
    if (it == struct_specializations_.end()) return nullptr;
    return select_spec_generic<ast::StructDecl>(*this, it->second, mod_.decls,
                                                args, out_params, out_args);
}

const ast::ClassDecl *TypeChecker::select_class_specialization(
    const std::string &base, const std::vector<Type> &args,
    std::vector<std::string> &out_params, std::vector<Type> &out_args) {
    auto it = class_specializations_.find(base);
    if (it == class_specializations_.end()) return nullptr;
    return select_spec_generic<ast::ClassDecl>(*this, it->second, mod_.decls,
                                               args, out_params, out_args);
}

const ast::FunctionDecl *TypeChecker::select_function_specialization(
    const std::string &base, const std::vector<Type> &args,
    std::vector<std::string> &out_params, std::vector<Type> &out_args) {
    auto it = function_specializations_.find(base);
    if (it == function_specializations_.end()) return nullptr;
    return select_spec_generic<ast::FunctionDecl>(*this, it->second, mod_.decls,
                                                  args, out_params, out_args);
}

} // namespace vex
