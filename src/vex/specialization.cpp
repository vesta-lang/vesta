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

} // namespace

const ast::StructDecl *TypeChecker::select_struct_specialization(
    const std::string &base, const std::vector<Type> &args,
    std::vector<std::string> &out_params, std::vector<Type> &out_args) {
    auto it = struct_specializations_.find(base);
    if (it == struct_specializations_.end()) return nullptr; // sin specs

    const ast::StructDecl *best = nullptr;
    int best_score = -1;
    std::unordered_map<std::string, Type> best_bindings;
    const ast::StructDecl *best_spec_for_params = nullptr;

    for (size_t idx : it->second) {
        if (idx >= mod_.decls.size()) continue;
        auto *spec = static_cast<const ast::StructDecl *>(mod_.decls[idx].get());
        if (!spec || !spec->is_specialization) continue;
        if (spec->spec_pattern.size() != args.size()) continue;

        std::unordered_set<std::string> fresh(spec->type_params.begin(),
                                              spec->type_params.end());
        std::unordered_map<std::string, Type> bindings;
        bool ok = true;
        for (size_t i = 0; i < args.size(); ++i) {
            if (!match_spec_pattern(*this, spec->spec_pattern[i].get(), args[i],
                                    fresh, bindings)) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        // Especificidad: TOTAL (sin params frescos) gana a PARCIAL.  Entre
        // dos del mismo rango se queda la primera declarada (no se espera
        // ambiguedad con los patrones soportados).
        const int score = spec->type_params.empty() ? 100 : 50;
        if (score > best_score) {
            best_score = score;
            best = spec;
            best_bindings = bindings;
            best_spec_for_params = spec;
        }
    }

    if (!best) return nullptr;

    // Rellenar los bindings en el orden de declaracion de los params frescos.
    out_params = best_spec_for_params->type_params;
    out_args.clear();
    out_args.reserve(out_params.size());
    for (const auto &p : out_params) {
        auto bit = best_bindings.find(p);
        out_args.push_back(bit != best_bindings.end() ? bit->second : Type{});
    }
    return best;
}

} // namespace vex
