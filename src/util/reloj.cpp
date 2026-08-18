/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file util/reloj.cpp
 * @brief Implementacion del reloj fino (ver @c util/reloj.h).
 */

#include "util/reloj.h"

#include <chrono>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
#define VESTA_RELOJ_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif
#endif

namespace util {
namespace reloj {

namespace {

#ifdef VESTA_RELOJ_X86
/**
 * @brief ¿Declara el procesador que su contador de ciclos es INVARIANTE?
 *
 * Es la unica pregunta que decide si el contador sirve como reloj: invariante
 * quiere decir que corre a ritmo fijo pase lo que pase con la frecuencia y sin
 * pararse al dormir el nucleo.  Sin esa garantia el contador mide CICLOS, y los
 * ciclos no son tiempo -- un tramo medido mientras el procesador baja de
 * frecuencia saldria mas corto de lo que fue.
 *
 * CPUID, hoja extendida 0x80000007, bit 8 de EDX.
 */
bool tsc_es_invariante() {
    unsigned a = 0, b = 0, c = 0, d = 0;
#if defined(_MSC_VER)
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000007u) return false;
    __cpuid(regs, 0x80000007);
    d = static_cast<unsigned>(regs[3]);
#else
    if (__get_cpuid_max(0x80000000u, nullptr) < 0x80000007u) return false;
    if (!__get_cpuid(0x80000007u, &a, &b, &c, &d)) return false;
#endif
    (void)a;
    (void)b;
    (void)c;
    return (d & (1u << 8)) != 0;
}

inline uint64_t leer_tsc() {
#if defined(_MSC_VER)
    return __rdtsc();
#else
    return __builtin_ia32_rdtsc();
#endif
}
#endif // VESTA_RELOJ_X86

inline uint64_t leer_steady() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// Estado del modulo: que reloj se usa y a cuanto tiempo equivale un tick.
struct Estado {
    bool usar_tsc = false;
    double ns_por_tick = 1.0;
    Info info;
};

/**
 * @brief Elige reloj, lo calibra y mide lo que se sabe de el.
 *
 * La calibracion del contador de ciclos se hace CONTRA el reloj del sistema:
 * se dejan pasar unos milisegundos y se mira cuantos ticks cupieron.  No hay
 * otra forma -- el procesador no dice a que ritmo corre su contador, y leerlo
 * de la frecuencia nominal seria justo el error que el bit de invarianza
 * advierte.
 */
Estado calibrar() {
    Estado e;
#ifdef VESTA_RELOJ_X86
    e.info.tsc_invariante = tsc_es_invariante();
    if (e.info.tsc_invariante) {
        const uint64_t t0 = leer_tsc();
        const auto w0 = std::chrono::steady_clock::now();
        /* Espera activa corta: dormir devolveria el control al sistema y el
         * intervalo real seria otro.  Cinco milisegundos bastan para que la
         * granularidad del reloj de referencia (100 ns) no pese. */
        std::chrono::steady_clock::time_point w1;
        do {
            w1 = std::chrono::steady_clock::now();
        } while (w1 - w0 < std::chrono::milliseconds(5));
        const uint64_t t1 = leer_tsc();
        const long long ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0)
                .count();
        const uint64_t ticks = t1 - t0;
        if (ticks > 0 && ns > 0) {
            e.usar_tsc = true;
            e.ns_por_tick =
                static_cast<double>(ns) / static_cast<double>(ticks);
            e.info.fuente = "tsc";
        }
    }
#endif
    if (!e.usar_tsc) {
        e.ns_por_tick = 1.0; // steady_clock ya se lee en nanosegundos.
        e.info.fuente = "steady_clock";
    }

    /* Resolucion: el MENOR salto observable, no el primero.
     *
     * Tomar el primero daba cifras que variaban doce veces entre dos corridas
     * del mismo binario: si justo en esa toma el sistema interrumpe, el salto
     * sale enorme y se guarda como si fuera la resolucion del reloj.  Lo que se
     * busca es el limite de lo que el reloj PUEDE distinguir, y ese es el
     * minimo: las muestras contaminadas solo pueden salir mas grandes. */
    for (int i = 0; i < 1000; ++i) {
        const uint64_t a = e.usar_tsc ?
#ifdef VESTA_RELOJ_X86
                                      leer_tsc()
#else
                                      leer_steady()
#endif
                                      : leer_steady();
        uint64_t b = a;
        int giros = 0;
        while (b == a && giros < 1000000) {
            b = e.usar_tsc ?
#ifdef VESTA_RELOJ_X86
                           leer_tsc()
#else
                           leer_steady()
#endif
                           : leer_steady();
            ++giros;
        }
        if (b > a) {
            /* Sin truncar: con el contador de ciclos un salto vale del orden de
             * 0,3 ns, y redondeado seria cero. */
            const double ns = static_cast<double>(b - a) * e.ns_por_tick;
            if (ns > 0.0 &&
                (e.info.resolucion_ns == 0.0 || ns < e.info.resolucion_ns))
                e.info.resolucion_ns = ns;
        }
    }

    /* Coste: lo que tarda una lectura, repartido sobre muchas para que la
     * propia granularidad no lo domine. */
    {
        constexpr int kN = 20000;
        const auto c0 = std::chrono::steady_clock::now();
        uint64_t sumidero = 0;
        /* Se lee el reloj DIRECTAMENTE, no por @ref ahora.  Esa consulta el
         * estado, y el estado es justo lo que se esta construyendo aqui:
         * volver a entrar en la inicializacion de un estatico local se queda
         * BLOQUEADO en su guarda -- no lento, parado.  Costo un test que se
         * colgaba nada mas arrancar. */
        for (int i = 0; i < kN; ++i) {
#ifdef VESTA_RELOJ_X86
            sumidero += e.usar_tsc ? leer_tsc() : leer_steady();
#else
            sumidero += leer_steady();
#endif
        }
        const auto c1 = std::chrono::steady_clock::now();
        const long long ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0)
                .count();
        e.info.coste_ns = ns / kN;
        /* Que el compilador no borre el bucle por no usarse el resultado. */
        if (sumidero == 0xFFFFFFFFFFFFFFFFull) e.info.coste_ns += 0;
    }
    return e;
}

const Estado &estado() {
    static const Estado e = calibrar();
    return e;
}

} // namespace

uint64_t ahora() {
#ifdef VESTA_RELOJ_X86
    /* La bandera se consulta una vez y queda en un estatico local: es una
     * lectura de memoria caliente frente a una llamada al sistema. */
    static const bool tsc = estado().usar_tsc;
    if (tsc) return leer_tsc();
#endif
    return leer_steady();
}

long long a_ns(uint64_t ticks) {
    const Estado &e = estado();
    if (!e.usar_tsc) return static_cast<long long>(ticks);
    return static_cast<long long>(static_cast<double>(ticks) * e.ns_por_tick);
}

const Info &info() {
    return estado().info;
}

} // namespace reloj
} // namespace util
