/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ir/parallel_for.cpp
 * @brief Implementacion del recorrido paralelo (contrato en paralelo.h).
 *
 * Esta separacion no es estilo: `ThreadPool.h` arrastra `windows.h`, que define
 * `VOID` como macro y rompe cualquier cabecera con un `enum class` que use ese
 * nombre.  Aqui dentro no molesta.
 */
#include "ir/parallel_for.h"

#include "util/ThreadPool.h"

#include <atomic>
#include <cstdlib>
#include <thread>

namespace ir {

unsigned compile_threads() {
    // Se lee UNA vez: consultar el entorno recorre su bloque entero, y esto se
    // pregunta en cada pase.
    static const unsigned n = [] {
        /* OPT-IN, no opt-out.  El reparto funciona -- el `.velb` sale identico
         * y el optimizador baja un 24% -- pero todavia cuelga en programas tan
         * pequenos como `15_herencia_basica.vx` (50 lineas), o sea que queda
         * estado compartido sin localizar en algun pase.
         *
         * Mientras eso no este cerrado, quien compile no puede pagarlo: un
         * camino nuevo entra activandose a mano y pasa a por defecto CUANDO
         * pasa el corpus entero, no antes.  Al reves es como se entrega un
         * compilador que se cuelga. */
        const char *e = std::getenv("VESTA_PARALELO");
        if (!e || e[0] == '0') return 1u;
        unsigned h = std::thread::hardware_concurrency();
        if (h <= 1) return 1u;
        // Uno menos que los nucleos: dejar la maquina sin margen hace que el
        // propio reparto compita con lo repartido.
        return h - 1;
    }();
    return n;
}

void for_each_function(IrModule &mod,
                       const std::function<void(IrFunction &)> &f) {
    const size_t n = mod.functions.size();
    const unsigned hilos = compile_threads();
    if (hilos <= 1 || n < 8) {
        for (auto &fn : mod.functions)
            f(fn);
        return;
    }
    /* Reparto por indice atomico y no por trozos iguales: las funciones no
     * cuestan lo mismo -- una de trescientas lineas junto a diez de cinco --,
     * asi que trocear a partes iguales deja hilos parados esperando al que le
     * toco la grande. */
    ThreadPool pool(hilos);
    std::atomic<size_t> siguiente{0};
    std::atomic<size_t> hechas{0};
    for (unsigned h = 0; h < hilos; ++h) {
        pool.enqueue([&] {
            for (;;) {
                const size_t i = siguiente.fetch_add(1);
                if (i >= n) break;
                f(mod.functions[i]);
                hechas.fetch_add(1);
            }
        });
    }
    // Se espera a que TODAS esten hechas, no a que el pool se vacie: una tarea
    // encolada que aun no arranco tambien cuenta, y salir antes dejaria el pase
    // a medias sin que nadie lo notara.
    while (hechas.load() < n)
        std::this_thread::yield();
}

} // namespace ir
