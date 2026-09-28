/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/checker/concept_conformance.cpp
 * @brief `struct S : C<...>` / `class X : C<...>`: que un struct o una clase
 *        cumpla los conceptos que declara, con sus argumentos (en un struct,
 *        tambien los heredados de una base @Abstract).
 *
 * Coste cero: la misma via que `where T: C`, sin vtable ni codigo.
 */

#include "vx/generics/concepts.h"
#include "vx/generics/generic_infer.h" // escribir un tipo como en el fuente
#include "vx/type_checker.h"

#include <unordered_map>
#include <unordered_set>

namespace vx {

namespace {

/// Los structs del modulo por nombre, para subir por la cadena de bases.
using StructIndex = std::unordered_map<std::string, ast::StructDecl *>;

/**
 * @brief Una clave que distingue `Da<i64>` de `Da<f64>`: el mismo concepto con
 *        argumentos distintos son dos obligaciones.
 * @param ref El concepto escrito.
 * @return La clave.
 */
std::string contract_key(const ast::ConceptRef &ref) {
    std::string key = ref.name.str();
    for (const auto &a : ref.args) {
        key += '|';
        key += generics::type_node_text(a.get());
    }
    return key;
}

/**
 * @brief Anade a @p out los conceptos que declara @p d: sus `interface_names`
 *        y su primer nombre tras `:` si no es un struct (entonces es un
 *        concepto: `struct S : View<T>` sin base).
 * @param d    El struct.
 * @param idx  Los structs del modulo.
 * @param out  Las obligaciones acumuladas.
 * @param seen Las ya anotadas (por @ref contract_key).
 */
void add_contracts_of(const ast::StructDecl &d, const StructIndex &idx,
                      std::vector<ast::ConceptRef> &out,
                      std::unordered_set<std::string> &seen) {
    for (const ast::ConceptRef &ref : d.interface_names)
        if (seen.insert(contract_key(ref)).second) out.push_back(ref);
    if (d.super_name.empty() || idx.find(d.super_name) != idx.end()) return;
    ast::ConceptRef super;
    super.name = util::InternedName::intern(d.super_name);
    super.args = d.super_args;
    super.loc = d.loc;
    if (seen.insert(contract_key(super)).second) out.push_back(std::move(super));
}

} // namespace

void TypeChecker::verify_declared_concept_conformance() {
    StructIndex idx;
    for (auto &d : mod_.decls)
        if (d && d->kind == ast::NodeKind::StructDecl) {
            auto *sd = static_cast<ast::StructDecl *>(d.get());
            idx[sd->name] = sd;
        }

    for (const auto &decl : mod_.decls) {
        if (!decl || decl->kind != ast::NodeKind::StructDecl) continue;
        auto *s = static_cast<ast::StructDecl *>(decl.get());
        /* La plantilla no se comprueba: sus argumentos hablan de `T`.  Cada
         * instancia recibe las clausulas con los argumentos ya sustituidos y
         * se comprueba ella. */
        if (!s->type_params.empty()) continue;

        /* Lo exigido: lo que declara el struct MAS lo de toda su cadena de
         * bases @Abstract.  Un @Abstract puede declarar un concepto sin
         * cumplirlo del todo: la obligacion pasa al derivado concreto. */
        std::vector<ast::ConceptRef> required;
        std::unordered_set<std::string> seen;
        std::unordered_set<std::string> seen_base;
        const ast::StructDecl *cur = s;
        while (cur != nullptr && seen_base.insert(cur->name).second) {
            add_contracts_of(*cur, idx, required, seen);
            if (cur->super_name.empty()) break;
            const auto it = idx.find(cur->super_name);
            if (it == idx.end()) break; // la "base" era un concepto: ya contado
            cur = it->second;
        }
        if (required.empty()) continue;

        Type st{PrimitiveKind::STRUCT};
        st.struct_name = s->name;
        /* Un @Abstract no se exige entero: difiere a sus derivados.  Solo se
         * comprueba que el concepto exista (un nombre mal escrito). */
        const PromiseDepth depth = s->is_abstract ? PromiseDepth::ExistenceOnly
                                                  : PromiseDepth::Full;
        for (const ast::ConceptRef &ref : required)
            check_concept_promise(st, ref, s->loc, ConceptPromise::Header,
                                  depth);
    }

    /* Y las CLASES: los conceptos que declaran tras `:` (separados de sus
     * interfaces al construir la tabla) se comprueban igual, sobre la clase. */
    for (const auto &kv : class_layouts_) {
        const ClassLayout &cl = kv.second;
        if (cl.declared_concepts.empty() || cl.is_interface) continue;
        Type ct{PrimitiveKind::CLASS};
        ct.struct_name = cl.name;
        for (const ast::ConceptRef &ref : cl.declared_concepts)
            check_concept_promise(ct, ref, ref.loc, ConceptPromise::Header,
                                  PromiseDepth::Full);
    }
}

void TypeChecker::check_concept_promise(const Type &t,
                                        const ast::ConceptRef &ref,
                                        const SourceLoc &loc,
                                        ConceptPromise promise,
                                        PromiseDepth depth) {
    const std::string &cname = ref.name.str();
    const std::string type_text = written_type_name(t);
    if (!is_concept_name(*this, cname)) {
        const std::string written = written_name(cname);
        if (promise == ConceptPromise::Impl)
            diags_.diag(loc, DiagLevel::ERR, "VX2140", {written, type_text});
        else
            diags_.diag(loc, DiagLevel::ERR, "VX2159", {type_text, written});
        return;
    }
    if (depth == PromiseDepth::ExistenceOnly) return;
    const ConceptArgs cargs = concept_ref_args(*this, ref, {}, {});
    if (comptime_eval_concept(*this, cname, t, cargs).satisfied) return;
    diags_.diag(loc, DiagLevel::ERR,
                promise == ConceptPromise::Impl ? "VX2139" : "VX2160",
                {type_text, written_concept(*this, cname, cargs)});
}

} // namespace vx
