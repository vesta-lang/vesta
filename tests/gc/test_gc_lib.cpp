/**
 * @file tests/gc/test_gc_lib.cpp
 * @brief Inc 0 de libvesta_gc: valida la C-ABI del GC global sin ProcessVM.
 *
 * Comprueba que el GcHeap opera ProcessVM-less: alloc devuelve handles
 * distintos, deref es estable (sin GC), el payload es escribible, collect no
 * crashea y conserva los objetos (Inc 0 = owner_proc_ null -> no colecta aun),
 * y un alloc masivo (que dispara minor_gc interno) no rompe.
 */

#include "vesta_gc/gc_lib.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

int main() {
    int fails = 0;
    auto check = [&](bool cond, const char *msg) {
        if (!cond) {
            std::printf("  FAIL: %s\n", msg);
            ++fails;
        }
    };

    vex_gc_init();

    // 1) alloc da handles distintos con punteros de payload distintos.
    uint32_t h1 = vex_gc_alloc(16);
    uint32_t h2 = vex_gc_alloc(32);
    uint8_t *p1 = vex_gc_deref(h1);
    uint8_t *p2 = vex_gc_deref(h2);
    check(p1 != nullptr && p2 != nullptr, "deref de handle recien alocado es NULL");
    check(p1 != p2, "dos allocs distintos comparten payload");

    // 2) el payload es escribible y deref es estable (sin GC entre medias).
    std::memset(p1, 0xAB, 16);
    std::memset(p2, 0xCD, 32);
    check(vex_gc_deref(h1) == p1, "deref no estable sin GC");
    check(p1[0] == 0xAB && p1[15] == 0xAB, "payload p1 corrupto");
    check(p2[0] == 0xCD && p2[31] == 0xCD, "payload p2 corrupto");

    // 3) live_count refleja al menos los 2 vivos.
    check(vex_gc_live_count() >= 2, "live_count < 2 tras 2 allocs");

    // 4) collect no crashea y (Inc 0, sin raices precisas) conserva los objetos.
    vex_gc_collect();
    check(vex_gc_deref(h1) != nullptr, "objeto colectado por error en Inc 0");

    // 5) alloc masivo: dispara minor_gc interno; no debe romper.
    for (int i = 0; i < 200000; ++i) {
        uint32_t h = vex_gc_alloc(24);
        uint8_t *p = vex_gc_deref(h);
        if (p) p[0] = static_cast<uint8_t>(i);
    }
    check(true, "alloc masivo (placeholder, llegar aqui sin crash ya es exito)");

    if (fails == 0)
        std::printf("GC LIB Inc 0: OK (alloc/deref/collect ProcessVM-less, %llu vivos)\n",
                    (unsigned long long)vex_gc_live_count());
    else
        std::printf("GC LIB Inc 0: %d FALLOS\n", fails);
    return fails;
}
