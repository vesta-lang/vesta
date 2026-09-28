/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file concept_defaults.cpp
 * @brief Lo que un concepto da al tipo que DECLARA cumplirlo: sus campos y
 *        sus metodos por defecto.
 *
 * `struct Slice<T> : View<Slice<T>, T>` no deberia tener que reescribir
 * `len()`, `is_empty()` o `first()` si el concepto ya sabe escribirlos sobre
 * los papeles que exige.  El concepto los trae con su cuerpo; el tipo que lo
 * declara los recibe con `Self` y los argumentos sustituidos, como si los
 * hubiera escrito.  Lo que el tipo SI escribe gana.
 *
 * Se hace sobre el arbol, antes de montar los layouts: asi lo inyectado es un
 * miembro mas y todo lo que viene despues -- el layout, la comprobacion de
 * los cuerpos, el bajado, el ASA -- lo trata igual que a lo escrito, sin un
 * caso aparte.  Lo que lo distingue es su procedencia (@c ast::MemberOrigin),
 * que es lo que mira quien necesita saberlo (el editor).
 */

#include "vx/type_checker.h"

#include "vx/generics/concepts.h"
#include "vx/generics/member_clone.h"

#include <memory>
#include <string>
#include <vector>

namespace vx {
namespace {

/**
 * @struct ConceptSubst
 * @brief La sustitucion de los parametros de un concepto para UN tipo: el
 *        primero es el propio tipo, los demas los argumentos que escribio.
 *
 * Por tipos ESCRITOS, no resueltos: aqui aun no hay layouts (ver
 * @c vxgen::GenSubst::arg_nodes).
 */
struct ConceptSubst {
    std::unique_ptr<ast::TypeNode> self;          ///< el tipo que lo declara
    std::vector<const ast::TypeNode *> arg_nodes; ///< self + argumentos
    vxgen::GenSubst g;
};

/**
 * @brief Prepara la sustitucion de @p cd para el tipo @p host_name con los
 *        argumentos de @p ref.
 * @param cd        El concepto.
 * @param host_name El tipo que lo declara (su nombre de declaracion).
 * @param ref       El concepto como lo escribio el tipo.
 * @param out       Recibe la sustitucion.
 * @return Falso si el numero de argumentos no casa (lo dice la conformidad).
 */
bool make_concept_subst(const ast::ConceptDecl &cd, const std::string &host_name,
                        const ast::ConceptRef &ref, ConceptSubst &out) {
    /* Sin parametros (`concept Contable { ... }`) no hay nada que sustituir:
     * sus miembros se copian tal cual. */
    if (cd.type_params.empty()) return ref.args.empty();
    if (cd.type_params.size() != ref.args.size() + 1) return false;
    auto self = std::make_unique<ast::NamedTypeNode>();
    self->loc = ref.loc;
    self->name = host_name;
    out.self = std::move(self);
    out.arg_nodes.clear();
    out.arg_nodes.reserve(cd.type_params.size());
    out.arg_nodes.push_back(out.self.get());
    for (const auto &a : ref.args) out.arg_nodes.push_back(a.get());
    out.g.params = &cd.type_params;
    out.g.arg_nodes = &out.arg_nodes;
    return true;
}

/**
 * @brief La procedencia de lo que inyecta @p ref.
 * @param ref      El concepto como lo escribio el tipo.
 * @param original Donde esta escrito el miembro en el concepto.
 * @return La procedencia.
 */
ast::MemberOrigin concept_origin(const ast::ConceptRef &ref,
                                 const SourceLoc &original) {
    ast::MemberOrigin o;
    o.kind = ast::MemberOriginKind::Concept;
    o.via = ref;
    o.original = original;
    return o;
}

/**
 * @brief Hay en @p ms un metodo con ese nombre y esa aridad?
 * @param ms    Los metodos.
 * @param name  El nombre.
 * @param arity El numero de parametros.
 * @return Cierto si lo hay.
 */
bool has_method(const std::vector<std::unique_ptr<ast::ClassMethodDecl>> &ms,
                const std::string &name, size_t arity) {
    for (const auto &m : ms)
        if (m && m->name == name && m->params.size() == arity) return true;
    return false;
}

/**
 * @brief Hay en @p fs un campo con ese nombre?
 * @tparam FieldDecl @c ast::StructFieldDecl o @c ast::ClassFieldDecl.
 * @param fs   Los campos.
 * @param name El nombre.
 * @return Cierto si lo hay.
 */
template <class FieldDecl>
bool has_field(const std::vector<FieldDecl> &fs, const std::string &name) {
    for (const FieldDecl &f : fs)
        if (f.name == name) return true;
    return false;
}

/**
 * @brief Un campo del concepto (forma de struct) como campo de CLASE.
 * @param f El campo del concepto.
 * @param g La sustitucion.
 * @return El campo de clase.
 */
ast::ClassFieldDecl class_field_from_concept(const ast::StructFieldDecl &f,
                                             const vxgen::GenSubst &g) {
    ast::ClassFieldDecl nf;
    nf.loc = f.loc;
    nf.name = f.name;
    nf.type = vxgen::clone_type_with_subst(f.type.get(), g);
    nf.init = vxgen::clone_expr(f.default_init.get(), g);
    nf.dir = f.dir;
    nf.is_static = f.is_static;
    return nf;
}

/**
 * @brief Los conceptos que nombra la cabecera de un tipo: el primero tras `:`
 *        (si es un concepto y no una base) y los demas.
 * @tparam Decl @c ast::StructDecl o @c ast::ClassDecl.
 * @param tc    El comprobador.
 * @param d     La declaracion.
 * @param first Almacen para el primero, que la cabecera guarda por partes
 *              (nombre y argumentos sueltos); vive lo que la lista devuelta.
 * @return Las referencias, en orden de escritura.
 */
template <class Decl>
std::vector<const ast::ConceptRef *> header_concepts(const TypeChecker &tc,
                                                     const Decl &d,
                                                     ast::ConceptRef &first) {
    std::vector<const ast::ConceptRef *> out;
    out.reserve(d.interface_names.size() + 1);
    if (!d.super_name.empty() && is_concept_name(tc, d.super_name)) {
        first.name = util::InternedName::intern(d.super_name);
        first.args = d.super_args;
        first.loc = d.loc;
        out.push_back(&first);
    }
    for (const ast::ConceptRef &r : d.interface_names) out.push_back(&r);
    return out;
}

} // namespace

void TypeChecker::inject_concept_defaults_into(ast::StructDecl &s) {
    if (!s.type_params.empty() || s.is_specialization) return; // plantilla
    ast::ConceptRef first;
    for (const ast::ConceptRef *ref : header_concepts(*this, s, first)) {
        const ast::ConceptDecl *cd = find_user_concept(*this, ref->name.str());
        if (cd == nullptr || cd->ckind != ast::ConceptKind::Structural)
            continue;
        ConceptSubst cs;
        if (!make_concept_subst(*cd, s.name, *ref, cs)) continue;
        for (const ast::StructFieldDecl &f : cd->fields) {
            if (has_field(s.fields, f.name)) continue; // lo escrito gana
            ast::StructFieldDecl nf =
                vxgen::clone_struct_field_with_subst(f, cs.g);
            nf.origin = concept_origin(*ref, f.loc);
            s.fields.push_back(std::move(nf));
        }
        for (const auto &m : cd->methods) {
            if (!m || !m->body) continue; // el exigido no trae nada que dar
            if (has_method(s.methods, m->name, m->params.size())) continue;
            auto nm = vxgen::clone_method_with_subst(
                *m, cs.g, vxgen::MethodBodyCopy::Clone);
            nm->origin = concept_origin(*ref, m->loc);
            s.methods.push_back(std::move(nm));
        }
    }
}

void TypeChecker::inject_concept_defaults_into(ast::ClassDecl &c,
                                               const TypeDeclIndex *index) {
    if (!c.type_params.empty() || c.is_specialization) return; // plantilla
    TypeDeclIndex built; // solo si hace falta y no lo trajeron
    ast::ConceptRef first;
    for (const ast::ConceptRef *ref : header_concepts(*this, c, first)) {
        const ast::ConceptDecl *cd = find_user_concept(*this, ref->name.str());
        if (cd == nullptr || cd->ckind != ast::ConceptKind::Structural)
            continue;
        ConceptSubst cs;
        if (!make_concept_subst(*cd, c.name, *ref, cs)) continue;
        for (const ast::StructFieldDecl &f : cd->fields) {
            if (has_field(c.fields, f.name)) continue;
            ast::ClassFieldDecl nf = class_field_from_concept(f, cs.g);
            nf.origin = concept_origin(*ref, f.loc);
            c.fields.push_back(std::move(nf));
        }
        for (const auto &m : cd->methods) {
            if (!m || !m->body) continue;
            /* Lo escrito en la clase o en una base suya gana: si no, el
             * metodo del concepto redefiniria el de la base. */
            if (has_method(c.methods, m->name, m->params.size())) continue;
            if (!c.super_name.empty()) {
                if (index == nullptr) {
                    built = index_type_decls();
                    index = &built;
                }
                if (class_chain_has_method(*index, c.super_name, m->name,
                                           m->params.size()))
                    continue;
            }
            auto nm = vxgen::clone_method_with_subst(
                *m, cs.g, vxgen::MethodBodyCopy::Clone);
            nm->origin = concept_origin(*ref, m->loc);
            c.methods.push_back(std::move(nm));
        }
    }
}

void TypeChecker::inject_concept_defaults_into(ast::ImplDecl &im,
                                               const TypeDeclIndex &index) {
    const ast::ConceptDecl *cd = find_user_concept(*this, im.concept_name);
    if (cd == nullptr || cd->ckind != ast::ConceptKind::Structural) return;
    ast::ConceptRef ref;
    ref.name = util::InternedName::intern(im.concept_name);
    ref.args = im.concept_args;
    ref.loc = im.loc;
    ConceptSubst cs;
    if (!make_concept_subst(*cd, im.target_type, ref, cs)) return;
    /* El tipo destino: declarado en este modulo (su arbol) o importado (su
     * layout).  Aqui aun no hay layouts de lo local. */
    const ast::StructDecl *target_struct = nullptr;
    const auto it = index.find(im.target_type);
    if (it != index.end() && it->second->kind == ast::NodeKind::StructDecl)
        target_struct = static_cast<const ast::StructDecl *>(it->second);
    for (const auto &m : cd->methods) {
        if (!m || !m->body) continue;
        const size_t arity = m->params.size();
        if (has_method(im.methods, m->name, arity)) continue;
        if (target_struct != nullptr
                ? has_method(target_struct->methods, m->name, arity)
                : class_chain_has_method(index, im.target_type, m->name,
                                         arity))
            continue;
        auto nm = vxgen::clone_method_with_subst(*m, cs.g,
                                                 vxgen::MethodBodyCopy::Clone);
        nm->origin = concept_origin(ref, m->loc);
        im.methods.push_back(std::move(nm));
    }
}

TypeChecker::TypeDeclIndex TypeChecker::index_type_decls() const {
    TypeDeclIndex index;
    index.reserve(mod_.decls.size());
    for (const auto &d : mod_.decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::StructDecl)
            index.emplace(static_cast<const ast::StructDecl &>(*d).name,
                          d.get());
        else if (d->kind == ast::NodeKind::ClassDecl)
            index.emplace(static_cast<const ast::ClassDecl &>(*d).name,
                          d.get());
    }
    return index;
}

bool TypeChecker::class_chain_has_method(const TypeDeclIndex &index,
                                         const std::string &class_name,
                                         const std::string &name,
                                         size_t arity) const {
    std::string cur = class_name;
    for (int guard = 0; !cur.empty() && guard < 64; ++guard) {
        const ast::ClassDecl *local = nullptr;
        const auto it_local = index.find(cur);
        if (it_local != index.end() &&
            it_local->second->kind == ast::NodeKind::ClassDecl)
            local = static_cast<const ast::ClassDecl *>(it_local->second);
        if (local != nullptr) {
            if (has_method(local->methods, name, arity)) return true;
            cur = local->super_name;
            continue;
        }
        /* Importada: su layout ya viene hecho. */
        const auto it = class_layouts_.find(cur);
        if (it == class_layouts_.end()) return false;
        for (const ClassMethodInfo &mi : it->second.methods)
            if (mi.name == name && mi.param_types.size() == arity) return true;
        cur = it->second.super_name;
    }
    return false;
}

void TypeChecker::inject_concept_defaults() {
    const TypeDeclIndex index = index_type_decls();
    /* Por indice: inyectar no anyade declaraciones, pero asi no depende de
     * ello. */
    for (size_t i = 0; i < mod_.decls.size(); ++i) {
        ast::Node *d = mod_.decls[i].get();
        if (d == nullptr) continue;
        switch (d->kind) {
        case ast::NodeKind::StructDecl:
            inject_concept_defaults_into(static_cast<ast::StructDecl &>(*d));
            break;
        case ast::NodeKind::ClassDecl:
            inject_concept_defaults_into(static_cast<ast::ClassDecl &>(*d),
                                         &index);
            break;
        case ast::NodeKind::ImplDecl:
            inject_concept_defaults_into(static_cast<ast::ImplDecl &>(*d),
                                         index);
            break;
        default: break;
        }
    }
    concept_defaults_pass_ = ConceptDefaultsPass::Done;
}

} // namespace vx
