/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file util/crono_tramo.cpp
 * @brief El acumulador de tramos (ver @c util/crono_tramo.h).
 */

#include "util/crono_tramo.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace util {

namespace {
/* Con cerrojo porque los modulos se compilan EN PARALELO.  Se toma una vez por
 * tramo -- no por instruccion --, asi que cuesta nada al lado de lo que mide. */
struct Acumulador {
    std::mutex                                                        m;
    std::unordered_map<const char *, std::pair<long long, long long>> t;
};

Acumulador &acumulador() {
    static Acumulador a;
    return a;
}
} // namespace

void acumular_tramo(const char *etiqueta, long long us) {
    Acumulador                 &a = acumulador();
    std::lock_guard<std::mutex> lk(a.m);
    auto                       &e = a.t[etiqueta];
    e.first += us;
    e.second += 1;
}

std::vector<Tramo> tramos_medidos() {
    Acumulador                 &a = acumulador();
    std::lock_guard<std::mutex> lk(a.m);
    std::vector<Tramo>          v;
    v.reserve(a.t.size());
    for (const auto &kv : a.t)
        v.push_back({kv.first, kv.second.first, kv.second.second});
    std::sort(v.begin(), v.end(),
              [](const Tramo &x, const Tramo &y) { return x.us > y.us; });
    return v;
}

void reiniciar_tramos() {
    Acumulador                 &a = acumulador();
    std::lock_guard<std::mutex> lk(a.m);
    a.t.clear();
}

} // namespace util
