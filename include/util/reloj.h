/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file util/reloj.h
 * @brief El reloj mas fino que la maquina sepa dar, y lo que se sabe de el.
 *
 * Modulo APARTE y sin dependencias a proposito: medir bien no es cosa del
 * optimizador ni del que cronometra tramos, y cualquiera que quiera medir algo
 * corto deberia poder usar esto sin arrastrar nada.
 *
 * POR QUE NO BASTA `steady_clock`.  En Windows sale de
 * @c QueryPerformanceCounter, que en las maquinas actuales corre a 10 MHz: un
 * salto cada **100 ns**.  Un tramo de treinta nanosegundos repetido cien mil
 * veces -- justo el que hay que descubrir -- no se puede distinguir de cero con
 * esa granularidad.  El contador de ciclos del procesador da del orden de 0,3
 * ns por tick y ademas se lee mas barato.
 *
 * CUANDO SE USA EL CONTADOR DE CICLOS.  Solo si el procesador declara que su
 * contador es INVARIANTE: que no cambia de ritmo con la frecuencia ni se para
 * al dormir el nucleo.  Se pregunta por CPUID y NO se supone -- en un
 * procesador sin esa garantia el contador mide ciclos, que no son tiempo.  Si
 * no la hay, se cae a @c steady_clock, que siempre esta.
 *
 * EN LINUX no hace falta el atajo: @c clock_gettime(CLOCK_MONOTONIC) se
 * resuelve en espacio de usuario (vDSO) con resolucion de nanosegundo, asi que
 * @c steady_clock ya da lo que aqui se busca.  El contador de ciclos se usa
 * igual si esta disponible porque se lee mas barato, y la calibracion que
 * imprime @ref info permite COMPROBAR en cada plataforma que la precision es
 * la que se espera en vez de darla por hecha.
 */

#ifndef VESTA_UTIL_RELOJ_H
#define VESTA_UTIL_RELOJ_H

#include <cstdint>

namespace util {
namespace reloj {

/// Lo que se sabe del reloj en esta maquina.  Se ensena en la telemetria: una
/// cifra mas fina que la resolucion de su reloj no significa nada, y sin verlo
/// nadie puede saberlo.
struct Info {
    const char *fuente = "?"; ///< "tsc" o "steady_clock".
    /**
     * @brief Salto minimo observable, en nanosegundos.
     *
     * En coma flotante y no entero: con el contador de ciclos vale del orden de
     * 0,3 ns, y redondeado a entero saldria CERO -- justo la cifra que se usa
     * para decidir si una medida significa algo.  Un reloj que dice que su
     * resolucion es cero no se puede distinguir de uno roto.
     */
    double resolucion_ns = 0.0;
    long long coste_ns = 0; ///< lo que cuesta una lectura.
    bool tsc_invariante = false;
};

/// Lectura del reloj, en TICKS de su propia unidad (no en tiempo).
uint64_t ahora();

/// Convierte una diferencia de ticks a nanosegundos.
long long a_ns(uint64_t ticks);

/// Que reloj se esta usando y con que precision.  Calibrado una sola vez.
const Info &info();

} // namespace reloj
} // namespace util

#endif // VESTA_UTIL_RELOJ_H
