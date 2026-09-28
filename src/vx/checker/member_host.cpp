/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file member_host.cpp
 * @brief A que tipo le anyade metodos un `extension` o un `impl`.
 *
 * Registrarlos, comprobar sus cuerpos y bajarlos son tres pasadas distintas,
 * y cada una tenia su copia de esta busqueda.
 */

#include "vx/type_checker.h"

#include "vx/module/namespace_names.h"

namespace vx {
namespace {

/**
 * @brief Busca una clave entre los layouts de struct y de clase.
 * @param structs Layouts de struct.
 * @param classes Layouts de clase.
 * @param key     La clave.
 * @param out     Recibe el destino si esta.
 * @return Cierto si esta.
 */
bool find_host_key(const std::unordered_map<std::string, StructLayout> &structs,
                   const std::unordered_map<std::string, ClassLayout> &classes,
                   const std::string &key, TypeChecker::MemberHost &out) {
    if (structs.count(key) != 0) {
        out.key = key;
        out.kind = PrimitiveKind::STRUCT;
        return true;
    }
    if (classes.count(key) != 0) {
        out.key = key;
        out.kind = PrimitiveKind::CLASS;
        return true;
    }
    return false;
}

} // namespace

TypeChecker::MemberHost
TypeChecker::find_member_host(const std::string &written) const {
    MemberHost h;
    if (find_host_key(struct_layouts_, class_layouts_, written, h)) return h;
    /* Cualificado: `mod.Tipo` esta registrado con su nombre fisico. */
    const std::string physical = namespace_symbol_path(written);
    if (physical != written &&
        find_host_key(struct_layouts_, class_layouts_, physical, h))
        return h;
    /* Importado o por alias: lo sabe la resolucion de tipos. */
    const Type rt = resolve_type_string(written);
    if (rt.kind == PrimitiveKind::STRUCT || rt.kind == PrimitiveKind::CLASS)
        (void)find_host_key(struct_layouts_, class_layouts_, rt.struct_name, h);
    return h;
}

} // namespace vx
