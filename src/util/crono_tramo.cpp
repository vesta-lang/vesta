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
#include <vector>

namespace util {

namespace {
/* UNO POR HILO, sin cerrojo.  Lo habia con cerrojo, y eso vale mientras se
 * cronometren tramos gruesos; en cuanto se mide algo que ocurre CIEN MIL veces
 * con los modulos compilando en paralelo, la contencion del propio cerrojo se
 * cobra dentro del tramo que envuelve -- y entonces el instrumento mide sobre
 * todo a si mismo.  Paso de verdad: un tramo dio 20 s cuyo cuerpo medido por
 * dentro costaba 219 us.
 *
 * Con uno por hilo, medir cuesta dos lecturas de reloj y una busqueda en una
 * tabla que nadie mas toca.  Los hilos se suman al consultar, que ocurre una
 * vez. */
struct Acumulador {
    std::unordered_map<const char *, std::pair<long long, long long>> t;
};

/// Los de todos los hilos, para poder sumarlos.  El cerrojo se toma SOLO al
/// darse de alta un hilo (una vez por hilo) y al consultar.
std::mutex &registro_mutex() {
    static std::mutex m;
    return m;
}
std::vector<Acumulador *> &registro() {
    static std::vector<Acumulador *> v;
    return v;
}

Acumulador &mio() {
    /* Se reserva y NO se libera a proposito: vive lo que el proceso, y liberarlo
     * al morir el hilo dejaria al registro con un puntero colgando justo cuando
     * alguien podria estar sumando. */
    static thread_local Acumulador *a = [] {
        auto                       *p = new Acumulador();
        std::lock_guard<std::mutex> lk(registro_mutex());
        registro().push_back(p);
        return p;
    }();
    return *a;
}
} // namespace

void acumular_tramo(const char *etiqueta, long long us) {
    auto &e = mio().t[etiqueta];
    e.first += us;
    e.second += 1;
}

std::vector<Tramo> tramos_medidos() {
    std::lock_guard<std::mutex>                                       lk(registro_mutex());
    std::unordered_map<const char *, std::pair<long long, long long>> total;
    for (const Acumulador *a : registro())
        for (const auto &kv : a->t) {
            auto &e = total[kv.first];
            e.first += kv.second.first;
            e.second += kv.second.second;
        }
    std::vector<Tramo> v;
    v.reserve(total.size());
    for (const auto &kv : total)
        v.push_back({kv.first, kv.second.first, kv.second.second});
    std::sort(v.begin(), v.end(),
              [](const Tramo &x, const Tramo &y) { return x.us > y.us; });
    return v;
}

void reiniciar_tramos() {
    std::lock_guard<std::mutex> lk(registro_mutex());
    for (Acumulador *a : registro()) a->t.clear();
}

} // namespace util
