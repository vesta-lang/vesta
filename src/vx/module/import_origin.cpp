/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file import_origin.cpp
 * @brief La regla con que se califica lo que llega de otro modulo.
 * @see vx/module/import_origin.h
 */

#include "vx/module/import_origin.h"

#include "vx/module/namespace_names.h" // la forma fisica de un nombre con ruta

namespace vx {

ImportOrigin import_origin_of(const std::string &module_name) {
    ImportOrigin o;
    o.module = util::InternedName::intern(module_name);
    return o;
}

std::string imported_symbol(const ImportOrigin &origin,
                            const std::string &ns_path,
                            const std::string &name) {
    /* Ya calificado: la cadena de re-exports lo trae con su identidad
     * original, y volver a prefijarlo daria una identidad por eslabon
     * (`std__syscall__std__syscall__windows__std__ntwindows__std__types__
     * uintptr`), con la que el tipo deja de unificar consigo mismo.  El
     * criterio es el invariante del formato: un nombre publico corto nunca
     * lleva `__`, porque el exportador lo parte siempre en (namespace,
     * nombre corto). */
    if (name.find(kSymbolPathSeparator) != std::string::npos) return name;
    if (!ns_path.empty()) return namespace_member_symbol(ns_path, name);
    return qualified_symbol(origin.module.str(), name);
}

} // namespace vx
