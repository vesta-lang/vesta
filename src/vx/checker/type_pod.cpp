/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file type_pod.cpp
 * @brief Si un tipo es POD: sus bytes son su valor.
 *
 * La huella `@pod` de cada tipo lo calculaba a mano (y aparte para structs y
 * para enums), y no habia forma de pedirlo como concepto.  La regla vive aqui
 * y la usan los dos.
 */

#include "vx/type_checker.h"

namespace vx {

bool TypeChecker::type_is_pod(const Type &t) const {
    /* Un enum comparte especie con el struct pero no tiene layout de struct:
     * las dos reglas de abajo no lo encuentran.  Es POD si lo son todas sus
     * cargas (uno sin carga lo es). */
    if (t.kind == PrimitiveKind::STRUCT) {
        const auto it = enum_layouts_.find(t.struct_name);
        if (it != enum_layouts_.end()) {
            for (const auto &v : it->second.variants)
                for (const auto &ft : v.field_types)
                    if (!type_is_pod(ft)) return false;
            return true;
        }
    }
    return type_is_c_representable(t) && !type_is_managed(t);
}

} // namespace vx
