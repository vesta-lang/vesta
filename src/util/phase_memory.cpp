/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file util/phase_memory.cpp
 * @brief La frontera de fase: lo que la anterior solto, de vuelta al reparto.
 */

#include "util/phase_memory.h"

#include "util/alloc/host_allocator.h"
#include "util/alloc/sanitizer.h" // san_mark, para nombrar la fase que empieza
#include "util/env_flags.h"

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace util {

/**
 * @brief El cuerpo de la frontera, con la clase por la que empieza a barrer.
 *
 * Las dos entradas publicas se diferencian SOLO en ese numero, asi que va aqui
 * una vez: dos copias de esto se separarian en cuanto alguien anadiera un paso
 * a una, y la que se quedara corta no fallaria -- devolveria menos, callando.
 */
static void release_impl(const char *next_mark, uint32_t from_class) {
    /* Es EL sitio: el asignador documenta estas llamadas como "de entre fases
     * y no algo para un bucle caliente", y una frontera de fase es exactamente
     * eso.  El porque de cada una y el porque del orden, en la cabecera. */
    (void)host_span_trim();
    (void)host_chunk_reclaim(from_class);
    /* Y LAS CACHES QUE YA NO TIENEN DUENO, que aqui son la mayoria: el
     * compilador reparte los modulos entre un lote de hilos y esos hilos
     * MUEREN, dejando cada uno su cache con sus trozos.  El barrido de arriba
     * solo mira la del hilo que cruza la frontera -- el principal --, asi que
     * sin esto lo de los trabajadores no lo recoge nadie.  Medido: 185 MiB
     * quietos durante todo el pico, y la mitad de eso con un solo hilo. */
    (void)host_chunk_reclaim_idle(from_class);
    (void)host_span_release();

    /* PONERLE PRECIO AL BARRIDO, sin cambiar nada: cuenta los trozos que se
     * podrian devolver y cuanto tarda en contarlos, y no devuelve ni uno.
     * Sirve para saber si una frontera nueva paga ANTES de ponerla, que es la
     * pregunta que trae a alguien aqui.  Con `VESTA_ALLOC_SCAN=1`. */
    if (flag_on(FlagId::HostAllocScan)) {
        uint64_t blocks = 0, chunks = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const size_t bytes = host_chunk_scan(kReclaimFromClass, &blocks,
                                             &chunks);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr,
                     "[scan] %-18s %8llu bloques  %6llu trozos  %6.1f MiB"
                     "  %7lld us\n",
                     next_mark != nullptr ? next_mark : "(fin)",
                     (unsigned long long)blocks, (unsigned long long)chunks,
                     double(bytes) / (1024.0 * 1024.0), (long long)us);
    }

    if (next_mark != nullptr) san_mark(next_mark);
}

/**
 * @brief Por que clase empieza a barrer ESTE consumidor.
 *
 * El asignador trae 8 por defecto y dice que quien tenga otro perfil de
 * tamanos pase el suyo.  El del compilador es mas pequeno -- 114 bytes de
 * media, la mayoria por debajo de esa clase --, asi que 8 se deja fuera casi
 * todo lo suyo.
 *
 * MEDIDO SOBRE 144.000 LINEAS, 12 corridas intercaladas por punto:
 *
 *     desde clase  8   621,7 MiB   3.057 ms      (el defecto del asignador)
 *     desde clase  4   595,2 MiB   3.232 ms      -26 MiB por +4,4%
 *     desde clase  0   572,9 MiB   3.951 ms      -49 MiB por +29%
 *
 * El cuatro es donde gira la curva: el 55% de la memoria por el 15% del coste.
 * El cero no compensa porque el barrido cuesta por BLOQUE y devuelve por TROZO,
 * y en una clase de 16 bytes hay 4.096 bloques que tienen que estar TODOS
 * libres para que el trozo valga algo.
 *
 * No es un numero que se pueda dejar puesto y olvidar: depende del perfil de
 * tamanos de quien reserva.  Si el compilador cambia como reserva, esta tabla
 * caduca y hay que rehacerla -- son tres builds.
 */
constexpr uint32_t kCompilerReclaimFrom = 4;

void release_between_phases(const char *next_mark) {
    release_impl(next_mark, kCompilerReclaimFrom);
}

} // namespace util
