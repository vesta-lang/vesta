/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file member_visibility.cpp
 * @brief Si el codigo que se comprueba puede usar un miembro.
 *
 * Una sola regla para todos los miembros -- campos y metodos, de struct, de
 * overlay y de clase, de instancia y estaticos --.  Antes habia dos
 * comprobaciones sueltas, solo de `private` y solo en clases, que buscaban la
 * declaracion en el arbol por NOMBRE (y por eso no veian nada importado ni
 * heredado); todo lo demas -- `protected`, los structs, los estaticos -- se
 * aceptaba sin mirar.
 *
 * Lo que un miembro sabe de si mismo (su visibilidad, que tipo lo escribio)
 * viaja en su ficha; aqui solo se decide.
 */

#include "vx/type_checker.h"

#include "vx/diag/diag_catalog.h" // la palabra del miembro, traducida

namespace vx {

bool TypeChecker::struct_derives_from(const std::string &sub,
                                      const std::string &base) const {
    std::string cur = sub;
    for (int guard = 0; !cur.empty() && guard < 64; ++guard) {
        if (cur == base) return true;
        const auto it = struct_layouts_.find(cur);
        if (it == struct_layouts_.end()) return false;
        cur = it->second.super_name;
    }
    return false;
}

const std::string &TypeChecker::member_declared_in(const ClassMethodInfo &m) {
    return m.origin.kind == ast::MemberOriginKind::Inherited
               ? m.origin.via.name.str()
               : m.defining_class;
}

PooledName TypeChecker::method_code_owner(const ast::ClassMethodDecl &m,
                                          const std::string &layout) {
    if (m.origin.kind == ast::MemberOriginKind::Inherited)
        return PooledName(m.origin.via.name.str());
    return PooledName(layout);
}

bool TypeChecker::member_reachable(Visibility v,
                                   const std::string &declared_in) const {
    switch (v) {
    case Visibility::Public:
    case Visibility::Unwritten:
    case Visibility::Internal:
        /* El modulo y el paquete: lo que no sale de ahi no llega a otro
         * modulo por su interfaz. */
        return true;
    case Visibility::Private:
        return member_code_site_ == MemberCodeSite::TypeBody &&
               member_code_owner_ == declared_in;
    case Visibility::Protected: {
        if (member_code_site_ != MemberCodeSite::TypeBody ||
            member_code_owner_.empty())
            return false;
        const std::string &owner = member_code_owner_;
        return owner == declared_in || struct_derives_from(owner, declared_in) ||
               type_derives_from(owner, declared_in);
    }
    }
    return true;
}

namespace {

/**
 * @brief Dice que un miembro no se alcanza desde aqui.
 * @param diags       Diagnosticos.
 * @param loc         El uso.
 * @param kind_word   Codigo del catalogo con la palabra (`el campo`...).
 * @param v           La visibilidad del miembro.
 * @param name        El miembro.
 * @param declared_in El tipo que lo escribio, como se escribe.
 */
void report_unreachable(Diagnostics &diags, const SourceLoc &loc,
                        const char *kind_word, Visibility v,
                        const std::string &name,
                        const std::string &declared_in) {
    const std::string word = diag::format(kind_word);
    diags.diag(loc, DiagLevel::ERR,
               v == Visibility::Protected ? "VX2165" : "VX2164",
               {word, name, declared_in});
}

} // namespace

bool TypeChecker::check_member_visible(const StructFieldInfo &f,
                                       const SourceLoc &loc) {
    const std::string &owner =
        f.declared_in.empty() ? std::string() : f.declared_in.str();
    if (member_reachable(f.visibility, owner)) return true;
    report_unreachable(diags_, loc, "VX2166", f.visibility, f.name,
                       written_name(owner));
    return false;
}

bool TypeChecker::check_member_visible(const ClassMethodInfo &m,
                                       const SourceLoc &loc) {
    const std::string &owner = member_declared_in(m);
    if (member_reachable(m.visibility, owner)) return true;
    report_unreachable(diags_, loc, m.is_constructor ? "VX2168" : "VX2167",
                       m.visibility, written_name(m.name), written_name(owner));
    return false;
}

} // namespace vx
