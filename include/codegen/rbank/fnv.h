/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/fnv.h
 * @brief Util de hashing FNV-1a de 64 bits, compartido por quien computa huellas
 *        (identidad del problema, del banco, ...).  Sin dependencias.
 */

#ifndef VESTA_CODEGEN_RBANK_FNV_H
#define VESTA_CODEGEN_RBANK_FNV_H

#include <cstdint>

namespace codegen {
namespace rbank {

/** @brief Semilla y primo FNV-1a de 64 bits. */
static constexpr uint64_t kFnvOffset = 1469598103934665603ull;
static constexpr uint64_t kFnvPrime  = 1099511628211ull;

/** @brief Mezcla un entero de 64 bits (byte a byte) en un acumulador FNV-1a. */
inline uint64_t fnv_mix(uint64_t h, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= (v & 0xff);
        h *= kFnvPrime;
        v >>= 8;
    }
    return h;
}

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_FNV_H
