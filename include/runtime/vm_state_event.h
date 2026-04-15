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

#ifndef VM_STATE_EVENT_H
#define VM_STATE_EVENT_H

namespace runtime {
    class ProcessVM;

    /**
     * @brief Representa los posibles estados de los hilos de la VM.
     *
     * Cada hilo dentro de la VM puede encontrarse en uno de estos estados,
     * lo cual permite al planificador (scheduler) controlar su ejecución,
     * suspensión, bloqueo y finalización.
     */
    typedef enum vm_state {
        /**
         * @brief El hilo está listo para ejecutarse, pero no está corriendo.
         *
         * @details
         * Se usa cuando:
         *  - El hilo ha sido creado y ya tiene su contexto inicial preparado.
         *  - El hilo ha cedido voluntariamente la CPU (yield).
         *  - El hilo ha sido desbloqueado tras un evento o temporizador.
         *
         * Implica:
         *  - El scheduler puede seleccionarlo para ejecutarse?
         */
        READY,


        /**
         * @brief El hilo está actualmente ejecutándose.
         *
         * @details
         * Se usa cuando:
         *  - El scheduler selecciona este hilo en un tick.
         *
         * Implica:
         *  - El hilo tiene el control de la "CPU" virtual.
         *  - Puede cambiar a otro estado según avance la ejecución.
         */
        RUNNING,

        /**
         * @brief El hilo está bloqueado esperando un evento, tiempo o señal.
         *
         * @details
         * Se usa cuando:
         *  - El hilo recién creado aún no está listo para ejecutarse.
         *  - El hilo espera un evento externo (I/O, sincronización, etc.).
         *  - El hilo espera un temporizador o condición.
         *
         * Implica:
         *  - El hilo no será ejecutado hasta que sea desbloqueado.
         */
        BLOCKED,

        /**
         * @brief El hilo ha terminado su ejecución permanentemente.
         *
         * @details
         * Se usa cuando:
         *  - La función principal del hilo retorna.
         *  - El hilo finaliza por un error fatal.
         *  - El hilo se cierra explícitamente.
         *
         * Implica:
         *  - El hilo no volverá a ejecutarse.
         *  - Su stack y recursos pueden ser liberados.
         */
        DEAD,

        /**
         * @brief La VM está decodificando la instrucción obtenida.
         *
         * @details
         * Se usa cuando:
         *  - La instrucción ya fue leída y ahora se interpreta.
         *
         * Implica:
         *  - Se determina el opcode, operandos y modo de direccionamiento.
         *  - Se preparan los datos necesarios para la ejecución.
         */
        DECODE,

        /**
         * @brief La VM está ejecutando la instrucción decodificada.
         *
         * @details
         * Se usa cuando:
         *  - La instrucción ya fue decodificada y se está ejecutando.
         *
         * Implica:
         *  - Se modifican registros, memoria o estado interno.
         *  - Puede generar cambios de estado (WAIT_IO, BLOCKED, etc.).
         */
        EXECUTE,


        /**
         * @brief La VM está esperando una operación de entrada/salida.
         *
         * @details
         * Se usa cuando:
         *  - La instrucción requiere esperar un dispositivo o evento externo.
         *
         * Implica:
         *  - El hilo no puede continuar hasta que la operación I/O termine.
         *  - El scheduler puede ejecutar otros "hilos" mientras tanto.
         */
        WAIT_IO,

        /**
         * @brief La VM ha detenido su ejecución.
         *
         * @details
         * Se usa cuando:
         *  - La VM recibe una instrucción HALT.
         *  - Se detiene explícitamente por el usuario o el sistema.
         *
         * Implica:
         *  - La VM no ejecutará más instrucciones.
         *  - Puede ser reiniciada o destruida.
         */
        HALT,

        /**
         * El proceso es nuevo y a sido creado, no se a puesto aun en ejecuccion hasta que se marque como READY.
         */
        NEW,

        /**
         * Este no es un estado real, sino que se usa para obtener cuantos estados
         * existen en nuestra maquina de estados.
         */
        NUM_STATES
    } vm_state;

    /**
     * @brief Eventos que disparan transiciones en la máquina de estados de la VM.
     *
     * Cada evento representa un suceso interno del ciclo de ejecución
     * o una condición externa que obliga a cambiar de estado.
     */
    enum vm_event {
        /**
         * @brief El scheduler ha seleccionado este hilo para ejecutarse.
         *
         * @details
         * Este evento es generado por el planificador (scheduler) cuando decide
         * que la VM debe reanudar su ejecución. Normalmente se dispara cuando:
         *
         *  - La VM estaba en estado READY esperando turno.
         *  - La VM ha sido desbloqueada tras un WAIT_IO.
         *  - El sistema decide asignarle tiempo de CPU.
         *
         * Implica:
         *  - La VM pasa del estado READY al estado RUNNING.
         *  - El hilo virtual obtiene control de la "CPU" virtual.
         *  - El siguiente estado típico es FETCH, donde comienza el ciclo de instrucción.
         *
         * Este evento no es generado por la VM internamente, sino por el
         * scheduler externo que gestiona múltiples instancias de VM?
         */
        EVT_SCHEDULED,

        /**
         * Permite ceder control a otro proceso de forma voluntaria.
         */
        EVT_YIELD,

        /**
         * @brief La instrucción ha sido decodificada correctamente.
         *
         * @details
         * Se dispara cuando:
         *  - La fase DECODE identifica el opcode, operandos y modo de direccionamiento.
         *
         * Implica:
         *  - La VM puede pasar a EXECUTE para ejecutar la instrucción.
         */
        EVT_DECODE_DONE,

        /**
         * @brief La instrucción ha sido ejecutada completamente.
         *
         * @details
         * Se dispara cuando:
         *  - La fase EXECUTE finaliza sin requerir I/O ni bloquearse.
         *
         * Implica:
         *  - La VM puede volver a FETCH para obtener la siguiente instrucción.
         */
        EVT_EXEC_DONE,

        /**
         * @brief La instrucción requiere esperar una operación de entrada/salida.
         *
         * @details
         * Se dispara cuando:
         *  - La ejecución detecta que necesita esperar un dispositivo o evento externo.
         *
         * Implica:
         *  - La VM debe pasar a WAIT_IO hasta que el evento se complete.
         */
        EVT_IO_WAIT,

        /**
         * @brief La operación de entrada/salida ha finalizado.
         *
         * @details
         * Se dispara cuando:
         *  - Un dispositivo, hilo externo o evento notifica que la I/O terminó.
         *
         * Implica:
         *  - La VM puede volver a READY para ser planificada nuevamente.
         */
        EVT_IO_READY,

        /**
         * @brief Se ha ejecutado una instrucción HALT o se solicita detener la VM.
         *
         * @details
         * Se dispara cuando:
         *  - La instrucción HALT es encontrada.
         *  - El sistema solicita detener la ejecución.
         *
         * Implica:
         *  - La VM pasa al estado HALT y no ejecutará más instrucciones.
         */
        EVT_HALT,

        /**
         * @brief Ha ocurrido un error fatal durante la ejecución.
         *
         * @details
         * Se dispara cuando:
         *  - Se detecta un error irrecuperable (memoria inválida, instrucción ilegal, etc.).
         *
         * Implica:
         *  - La VM pasa al estado DEAD.
         *  - Se liberan recursos y se detiene la ejecución.
         */
        EVT_ERROR,

        /**
         * @brief Número total de eventos (no es un evento real).
         */
        NUM_EVENTS
    };

    static const char *event_name(vm_event e) {
        switch (e) {
            case EVT_DECODE_DONE: return "EVT_DECODE_DONE";
            case EVT_EXEC_DONE: return "EVT_EXEC_DONE";
            case EVT_IO_WAIT: return "EVT_IO_WAIT";
            case EVT_IO_READY: return "EVT_IO_READY";
            case EVT_HALT: return "EVT_HALT";
            case EVT_ERROR: return "EVT_ERROR";
            case EVT_SCHEDULED: return "EVT_SCHEDULED";
            default: return "UNKNOWN_EVENT";
        }
    }

    /**
     * Estados de la la FSM(Maquina de estasdos finitio) en la VM
     * @param state estado de la VM
     * @return representacion string
     */
    static const char *vm_state_to_str(vm_state state) {
        switch (state) {
            case READY: return "READY";
            case RUNNING: return "RUNNING";
            case BLOCKED: return "BLOCKED";
            case DECODE: return "DECODE";
            case EXECUTE: return "EXECUTE";
            case WAIT_IO: return "WAIT_IO";
            case HALT: return "HALT";
            case DEAD: return "DEAD";
            default: return "UNKNOWN";
        }
    }


    /**
     * Estructura que representa una transicion de la maquina de estados.
     */
    typedef struct Transition {
        vm_state                         next;
        std::function<void(ProcessVM *)> action;

        /**
         * Constructor de transicion, si no es especifica el estado siempre
         * sera READY.
         * @param n estado de transicion
         * @param a accion de transicion.
         */
        Transition(vm_state n = READY, void (*a)(ProcessVM *) = nullptr)
            : next(n), action(a) {}
    } Transition;


}

#endif //VM_STATE_EVENT_H
