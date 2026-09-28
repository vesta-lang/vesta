/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vxi_members.cpp
 * @brief Quien ve un miembro exportado y que tipo lo escribio: al escribirlo
 *        en el `.vxi` y al leerlo de vuelta.
 */

#include "vx/module/vxi_members.h"

namespace vx {

VxiMemberAttrs vxi_member_attrs_of(Visibility v, const std::string &declared_in,
                                   const std::string &type_key) {
    VxiMemberAttrs attrs;
    attrs.visibility = v;
    /* Lo escrito por el propio tipo -- casi todo -- no se apunta: el lector
     * entiende el vacio como "el tipo que lo exporta". */
    if (!declared_in.empty() && declared_in != type_key)
        attrs.declared_in = util::InternedName::intern(declared_in);
    return attrs;
}

std::string imported_declared_in(util::InternedName written,
                                 const Type &resolved,
                                 const std::string &type_key) {
    if (written.empty()) return type_key;
    const bool is_type = resolved.kind == PrimitiveKind::STRUCT ||
                         resolved.kind == PrimitiveKind::CLASS;
    if (is_type && !resolved.struct_name.empty())
        return resolved.struct_name;
    return type_key;
}

void stamp_imported_method_owner(ClassMethodInfo &cmi,
                                 const std::string &declared_in,
                                 const std::string &type_key) {
    if (declared_in == type_key) return;
    cmi.origin.kind = ast::MemberOriginKind::Inherited;
    cmi.origin.via.name = util::InternedName::intern(declared_in);
}

} // namespace vx
