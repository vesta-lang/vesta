/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file capabilities.cpp
 * @brief Que se puede responder, que no, y por que.
 */

#include "vxdbg/capabilities.h"

namespace vxdbg {

namespace {

/// Indice de una capacidad en las tablas planas: cuantos bits hay que
/// desplazar.  Las banderas son potencias de dos, asi que es su logaritmo.
size_t index_of(Capability c) {
    uint32_t v = static_cast<uint32_t>(c);
    size_t i = 0;
    while (v > 1) {
        v >>= 1;
        ++i;
    }
    return i;
}

} // namespace

CapabilitySet prerequisites(Capability c) {
    switch (c) {
    // Donde vive una variable no significa nada si no se pueden enumerar.
    case Capability::VariableLocation:
        return static_cast<CapabilitySet>(Capability::LiveVariables);
    // Y enumerarlas exige saber en que sentencia estamos, que es lo que acota
    // cuales tienen valor.
    case Capability::LiveVariables:
        return static_cast<CapabilitySet>(Capability::MapToStatement);
    // La jerarquia de un tipo exige haber resuelto a quien pertenece el codigo.
    case Capability::TypeHierarchy:
        return static_cast<CapabilitySet>(Capability::ResolveEntity);
    // Subir a la sentencia pasa por el intermedio: es la capa de en medio.
    case Capability::MapToStatement:
        return static_cast<CapabilitySet>(Capability::MapToIr);
    // Y llegar al intermedio exige haber localizado el codigo.
    case Capability::MapToIr:
    case Capability::ResolveEntity:
        return static_cast<CapabilitySet>(Capability::LocateCode);
    // El fichero acompana a la linea: darlo sin ella no dice donde.
    case Capability::SourceFile:
        return static_cast<CapabilitySet>(Capability::SourcePosition);
    case Capability::SourcePosition:
        return static_cast<CapabilitySet>(Capability::MapToStatement);
    // Reconstruir marcos incorporados exige saber de que intermedio se trata.
    case Capability::InlineFrames:
        return static_cast<CapabilitySet>(Capability::MapToIr);
    default: return 0;
    }
}

void Query::close_prerequisites() {
    // Punto fijo: se repite hasta que el conjunto deja de crecer.  Una sola
    // pasada cerraria un nivel y dejaria consultas que dicen poder
    // satisfacerse y luego no -- `VariableLocation` arrastra `LiveVariables`,
    // que arrastra `MapToStatement`, que arrastra `MapToIr` y `LocateCode`.
    CapabilitySet anterior = 0;
    while (anterior != needs) {
        anterior = needs;
        for (size_t i = 0; i < CAPABILITY_COUNT; ++i) {
            const auto c = static_cast<Capability>(1u << i);
            if (has(needs, c)) needs |= prerequisites(c);
        }
    }
}

Unavailable CapabilityReport::why_not(Capability c) const {
    if (can(c)) return Unavailable::Available;
    const size_t i = index_of(c);
    if (i >= CAPABILITY_COUNT) return Unavailable::Unsupported;
    return reasons_[i];
}

void CapabilityReport::offer(Capability c) {
    available_ |= c;
    const size_t i = index_of(c);
    if (i < CAPABILITY_COUNT) reasons_[i] = Unavailable::Available;
}

void CapabilityReport::deny(Capability c, Unavailable reason) {
    available_ &= ~c;
    const size_t i = index_of(c);
    if (i < CAPABILITY_COUNT) reasons_[i] = reason;
}

bool CapabilityReport::satisfies(const Query &q,
                                 CapabilitySet &out_missing) const {
    // Solo lo EXIGIDO decide.  Lo deseable que falte no invalida la consulta:
    // esa es toda la diferencia entre `needs` y `wants`.
    out_missing = q.needs & static_cast<CapabilitySet>(~available_);
    return out_missing == 0;
}

} // namespace vxdbg
