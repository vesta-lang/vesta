/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file util/fnv.h
 * @brief Hashing FNV-1a, compartido por quien computa huellas.
 *
 * Vivia dentro del asignador de banco ancho, que fue quien lo necesito primero.
 * Al hacerle falta tambien al subsistema de depuracion habia dos salidas malas
 * -- que la depuracion dependiera del asignador de registros, o copiar el
 * algoritmo -- y una buena: subirlo a un sitio comun.  Quien lo usaba antes lo
 * sigue viendo donde estaba.
 *
 * FNV-1a no es criptografico y aqui no hace falta que lo sea: se usa para dar
 * identidad a datos propios, no para resistir a nadie que intente provocar una
 * colision.
 */

#ifndef VESTA_UTIL_FNV_H
#define VESTA_UTIL_FNV_H

#include <cstddef>
#include <cstdint>

namespace util {

/** @brief Semilla y primo FNV-1a de 64 bits. */
static constexpr uint64_t kFnvOffset = 1469598103934665603ull;
static constexpr uint64_t kFnvPrime = 1099511628211ull;

/**
 * @brief Mezcla un entero de 64 bits (byte a byte) en un acumulador.
 * @param h Acumulador.
 * @param v Valor a mezclar.
 * @return El acumulador actualizado.
 */
inline uint64_t fnv_mix(uint64_t h, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= (v & 0xff);
        h *= kFnvPrime;
        v >>= 8;
    }
    return h;
}

/**
 * @brief Mezcla una secuencia de bytes en un acumulador.
 * @param h Acumulador.
 * @param data Bytes.
 * @param size Cuantos.
 * @return El acumulador actualizado.
 */
inline uint64_t fnv_bytes(uint64_t h, const void *data, size_t size) noexcept {
    const auto *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
    return h;
}

} // namespace util

#endif // VESTA_UTIL_FNV_H
