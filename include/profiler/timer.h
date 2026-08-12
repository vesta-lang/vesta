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
 * @file timer.h
 * @brief Temporizador de alta resolucion para medir tiempos de ejecucion en
 * VestaVM.
 *
 * Proporciona la estructura @c Timer basada en @c std::chrono::steady_clock,
 * que garantiza mediciones monoton icas (no retrocede, no se ve afectada por
 * ajustes del reloj del sistema) y de alta resolucion.
 *
 * Unidades disponibles y cuando usarlas:
 *
 *  - @b ms (milisegundos, 1 ms = 1.000 us):
 *      Util para medir tareas "grandes" como lectura de archivos o parseo de
 * proyectos completos.  Puede mostrar 0 si la operacion dura menos de 1 ms.
 *
 *  - @b us (microsegundos, 1 us = 1.000 ns):
 *      Precision media; ideal para medir fases del compilador (lexer, parser,
 *      ensamblador) o funciones de coste medio.
 *
 *  - @b ns (nanosegundos, 1 ns = 1/1.000.000.000 s):
 *      Maxima precision; util para micro-optimizaciones y benchmarks de
 * instrucciones individuales.  El resultado puede ser ruidoso en sistemas con
 * alta contention.
 *
 * Uso tipico:
 * @code
 *   Timer t;
 *   do_something();
 *   long long elapsed_us = t.us();
 *   printf("Elapsed: %lld us\n", elapsed_us);
 *
 *   t.reset();  // reinicia el punto de inicio
 *   do_something_else();
 *   printf("Elapsed: %lld ns\n", t.ns());
 * @endcode
 */

#ifndef TIMER_H
#define TIMER_H

#include "util/reloj.h"

#include <chrono>
#include <cstdint>

/**
 * @struct Timer
 * @brief Temporizador simple de alta resolucion con inicio automatico.
 *
 * El reloj arranca en el momento de la construccion del objeto o al llamar a
 * @c reset().  Las funciones @c ms(), @c us() y @c ns() miden el tiempo
 * transcurrido desde ese punto hasta el instante de la llamada.
 *
 * Usa el reloj del proyecto (@c util::reloj), que en x86 con TSC invariante da
 * ~5 ns de resolucion y ~5 ns por lectura, frente a los ~100 ns de resolucion y
 * ~50 ns por lectura del de la biblioteca estandar.  Eso importa aqui: quien
 * mide algo corto y muy repetido -- un quantum del planificador, un despacho --
 * con un reloj de 100 ns esta midiendo sobre todo el reloj.  Sigue siendo
 * monotono: no retrocede aunque se ajuste la hora del sistema.
 */
typedef struct Timer {
    /// Punto de inicio, en TICKS del reloj del proyecto.
    uint64_t start;

    /**
     * @brief Constructor: inicia el temporizador en el instante de creacion.
     */
    Timer() { reset(); }

    /**
     * @brief Reinicia el punto de inicio al instante actual.
     *
     * Llamar a @c reset() descarta el tiempo acumulado y comienza una nueva
     * medicion.
     */
    void reset() { start = util::reloj::ahora(); }

    /**
     * @brief Devuelve el tiempo transcurrido en milisegundos.
     *
     * Puede retornar 0 si la operacion duro menos de 1 ms.
     *
     * @return Tiempo transcurrido desde @c start en milisegundos (long long).
     */
    long long ms() const { return ns() / 1000000; }

    /**
     * @brief Devuelve el tiempo transcurrido en microsegundos.
     *
     * Precision adecuada para la mayoria de fases del compilador.
     *
     * @return Tiempo transcurrido desde @c start en microsegundos (long long).
     */
    long long us() const { return ns() / 1000; }

    /**
     * @brief Devuelve el tiempo transcurrido en nanosegundos.
     *
     * Maxima precision disponible; el resultado puede ser ruidoso en
     * sistemas bajo alta carga o con planificacion no determinista.
     *
     * @return Tiempo transcurrido desde @c start en nanosegundos (long long).
     */
    long long ns() const { return util::reloj::a_ns(util::reloj::ahora() - start); }
} Timer;

#endif // TIMER_H
