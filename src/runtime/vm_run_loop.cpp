/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

#include "runtime/runtime.h"

namespace runtime {
    void VM::run_loop() {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpedantic"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
        /**
         * Usar la tecnica "computed goto" no me lo permite el estandar,
         * asi que debo desabilitar aqui los warnings que genere el compilador
         * para que sea compilable
         */
        static void *dispatch_table[NUM_STATES] = {
            &&READY_LABEL,
            &&RUNNING_LABEL,
            &&BLOCKED_LABEL,
            &&DEAD_LABEL,
            &&FETCH_LABEL,
            &&DECODE_LABEL,
            &&EXECUTE_LABEL,
            &&WAIT_IO_LABEL,
            &&HALT_LABEL
        };

        goto *dispatch_table[state];

    READY_LABEL:
        // READY no ejecuta instrucciones, solo espera scheduling externo
        on_event(EVT_SCHEDULED); // transición READY -> RUNNING
        goto *dispatch_table[state];

    RUNNING_LABEL:
        // RUNNING simplemente pasa a FETCH
        on_event(EVT_SCHEDULED); // o EVT_SCHEDULED ?
        goto *dispatch_table[state];

    BLOCKED_LABEL:
        // No hay nada que hacer hasta que un evento externo desbloquee
        return;

    FETCH_LABEL:
        /*fetch_instruction();*/
        on_event(EVT_FETCH_DONE);
        goto *dispatch_table[state];

    DECODE_LABEL:
        on_event(EVT_DECODE_DONE);
        goto *dispatch_table[state];

    EXECUTE_LABEL:
        // si la instruccion ejecuta no es bloqueante, se avanzara en el
        // estado de la VM, pero en caso de que execute_instruction devuelva
        // false, se lanzara un evento de tipo EVT_IO_WAIT como evento bloqueante.
        on_event(execute_instruction());
        goto *dispatch_table[state];

    WAIT_IO_LABEL:
        /*if (io_ready())
            on_event(EVT_IO_READY);*/
        goto *dispatch_table[state];

    HALT_LABEL:
    DEAD_LABEL:
        return;
    }

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}
