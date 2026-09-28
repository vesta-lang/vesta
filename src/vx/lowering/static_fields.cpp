/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file static_fields.cpp
 * @brief Donde vive un campo `static`: en el tipo que lo DECLARA.
 *
 * Un derivado ve los estaticos de su base (segun su visibilidad) y son el
 * MISMO almacen, no una copia: `Base.x` y `Derivada.x` son la misma variable.
 * Antes un struct derivado pedia una global `Derivada__x` que nadie creaba, y
 * una clase derivada tenia su propia copia de los estaticos de la base.
 *
 * El tipo que declara cada campo lo apunta el comprobador en su ficha
 * (@c StructFieldInfo::declared_in); aqui solo se lee.
 */

#include "vx/lowering.h"

#include "ir/synthetic_symbols.h"

namespace vx {

Lowering::ClassStaticField Lowering::find_class_static_(const std::string &cls,
                                                        const std::string &field,
                                                        const SourceLoc &loc) {
    ClassStaticField out;
    const auto it = tc_.class_layouts().find(cls);
    if (it == tc_.class_layouts().end()) {
        error_at(loc, "lowering: clase desconocida '" + cls + "'");
        return out;
    }
    for (const StructFieldInfo &f : it->second.static_fields) {
        if (f.name != field) continue;
        out.field = &f;
        out.owner = f.declared_in.empty() ? cls : f.declared_in.str();
        return out;
    }
    error_at(loc, "lowering: static field '" + field +
                      "' no encontrado en la clase '" + cls + "'");
    return out;
}

std::string Lowering::struct_static_global_(const std::string &st,
                                            const std::string &field) const {
    const auto it = tc_.struct_layouts().find(st);
    if (it != tc_.struct_layouts().end())
        for (const StructFieldInfo &f : it->second.static_fields)
            if (f.name == field && !f.declared_in.empty())
                return ir::struct_static_symbol(f.declared_in.str(), field);
    return ir::struct_static_symbol(st, field);
}

} // namespace vx
