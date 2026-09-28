/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file generic_template_key.cpp
 * @brief De como se ESCRIBIO una plantilla a la clave con la que esta
 *        registrada.
 *
 * Una plantilla tiene una sola identidad, la de su origen, y se puede escribir
 * de varias formas segun como se importo.  Cada monomorfizacion y cada tipo
 * generico escrito convertian el TEXTO en nombre de instancia por su cuenta
 * (`lib.Caja` -> `lib_Caja_i64`, `Caja` -> `Caja_i64`), asi que la misma
 * instancia era un tipo distinto en cada modulo.  Aqui se resuelve la clave, y
 * el nombre de la instancia sale de ella.
 */

#include "vx/type_checker.h"

#include "vx/module/namespace_names.h" // la forma fisica de un nombre con ruta

namespace vx {

void TypeChecker::register_generic_type_alias(const std::string &public_name,
                                              const std::string &key) {
    if (public_name.empty() || public_name == key) return;
    generic_type_public_names_[util::InternedName::intern(public_name)] =
        util::InternedName::intern(key);
}

std::string TypeChecker::generic_template_key(
    const std::string &written,
    const std::unordered_map<std::string, size_t> &templates) const {
    if (templates.count(written) != 0) return written;
    if (written.find('.') != std::string::npos) {
        /* Cualificado: por la tabla de namespaces, que es la que sabe que
         * `L` es el alias local de `lib` -- convertir el texto (`L.Caja` ->
         * `L__Caja`) no lo sabe -- y a que clave lleva cada miembro. */
        uint32_t ns_idx = UINT32_MAX;
        std::string member;
        if (resolve_ns_qualified(written, ns_idx, member) &&
            ns_idx < imported_namespaces_.size()) {
            const ImportedNamespace &ns = imported_namespaces_[ns_idx];
            const auto its = ns.by_name.find(member);
            if (its != ns.by_name.end()) {
                const std::string &label = ns.symbols[its->second].mangled_label;
                if (templates.count(label) != 0) return label;
            }
        }
        /* Un namespace del PROPIO fichero no pasa por esa tabla: el aplanado
         * lo deja como `col__Box`. */
        const std::string flat = namespace_symbol_path(written);
        if (templates.count(flat) != 0) return flat;
        return std::string();
    }
    /* Nombre corto de una plantilla importada: su alias registrado. */
    const auto ia = generic_type_public_names_.find(util::InternedName::intern(written));
    if (ia != generic_type_public_names_.end() &&
        templates.count(ia->second.str()) != 0)
        return ia->second.str();
    /* Nombre corto de una plantilla de un namespace del PROPIO fichero
     * (`Box` dentro de `namespace col;`, registrada como `col__Box`): la
     * unica clave que termina en `__Box`.  Solo para eso: lo importado ya
     * tiene su alias arriba. */
    const std::string suffix = std::string(kSymbolPathSeparator) + written;
    std::string hit;
    int found = 0;
    for (const auto &kv : templates) {
        const std::string &k = kv.first;
        if (k.size() > suffix.size() &&
            k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0) {
            hit = k;
            if (++found > 1) break;
        }
    }
    return found == 1 ? hit : std::string();
}

std::string TypeChecker::generic_type_key(const std::string &written) const {
    std::string key = generic_template_key(written, generic_struct_templates_);
    if (key.empty()) key = generic_template_key(written, generic_templates_);
    if (key.empty()) key = generic_template_key(written, generic_enum_templates_);
    return key;
}

} // namespace vx
