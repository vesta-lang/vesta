/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ids.cpp
 * @brief Identidad por contenido: huellas de 128 bits.
 *
 * **No son criptograficas y no lo pretenden.**  Ver 128 bits invita a suponer
 * lo que dan BLAKE3 o SHA-256, y aqui no se busca eso: se busca que dos nodos
 * distintos no coincidan por accidente al identificarlos.  Nadie va a intentar
 * provocar una colision a proposito -- los datos son propios, generados por el
 * compilador --, asi que resistir a un adversario seria pagar por algo que no
 * hace falta.  Si algun dia estos nodos vinieran de fuera, esto habria que
 * cambiarlo.
 */

#include "vxdbg/ids.h"

#include "util/fnv.h"

#include <cstdio>

namespace vxdbg {

ContentHash hash_bytes(const void *data, size_t size) {
    ContentHash h;
    // Dos acumuladores con semillas distintas.  Uno de 64 bits basta para una
    // tabla, pero aqui la huella ES la identidad: dos nodos que colisionen
    // serian el mismo nodo para el almacen, y con 64 bits eso empieza a ser
    // probable a partir de unos pocos millones.  Con 128 deja de serlo.
    h.lo = util::fnv_bytes(util::kFnvOffset, data, size);
    h.hi =
        util::fnv_bytes(util::kFnvOffset ^ 0x9E3779B97F4A7C15ull, data, size);
    // La segunda pasada rompe la simetria: sin ella, dos entradas que difieren
    // en un solo byte al final producen huellas cuyas dos mitades varian igual,
    // y la de 128 bits no valdria mucho mas que la de 64.
    h.hi = util::fnv_mix(h.hi, h.lo);
    return h;
}

ContentHash hash_combine(ContentHash acc, ContentHash h) {
    ContentHash r;
    r.lo = util::fnv_mix(util::fnv_mix(acc.lo, h.lo), h.hi);
    r.hi = util::fnv_mix(util::fnv_mix(acc.hi, h.hi), h.lo);
    return r;
}

std::string ContentHash::to_hex() const {
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buf, 32);
}

ContentHash ContentHash::from_hex(const std::string &hex) {
    ContentHash h;
    if (hex.size() != 32) return h; // no tiene la forma: se devuelve vacia
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    // Una mitad por bucle.  Construirlas a la vez cuadraba pero obligaba a
    // llevar dos indices en la cabeza para comprobar que cada digito va donde
    // toca; asi se lee de corrido.
    uint64_t hi = 0, lo = 0;
    for (size_t i = 0; i < 16; ++i) {
        const int d0 = nibble(hex[i]);
        if (d0 < 0) return ContentHash{}; // un digito invalido: huella vacia
        hi = (hi << 4) | static_cast<uint64_t>(d0);
    }
    for (size_t i = 16; i < 32; ++i) {
        const int d0 = nibble(hex[i]);
        if (d0 < 0) return ContentHash{};
        lo = (lo << 4) | static_cast<uint64_t>(d0);
    }
    h.hi = hi;
    h.lo = lo;
    return h;
}

} // namespace vxdbg
