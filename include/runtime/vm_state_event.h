/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file vm_state_event.h
 * @brief Definicion de los estados y eventos del proceso virtual de VestaVM.
 *
 * Define la enumeracion @c VMStateEvent que representa las transiciones
 * de estado de un proceso: NEW, READY, RUNNING, BLOCKED, WAITING, DEAD.
 */#ifndef VM_STATE_EVENT_H
#define VM_STATE_EVENT_H

namespace runtime {
    class ProcessVM; ///< Proceso virtual (declaracion adelantada)

    /**
     * @brief Estados posibles de un proceso virtual en la FSM del scheduler.
     *
     * La FSM del scheduler controla el ciclo de vida de cada proceso virtual.
     * Los estados se organizan segun la fase del ciclo de instruccion o la
     * condicion de bloqueo del proceso:
     *
     *   NEW -> READY -> RUNNING -> DECODE -> EXECUTE -> READY (ciclo normal)
     *                           -> EXECUTE -> WAIT_IO -> READY (I/O)
     *                           -> EXECUTE -> BLOCKED -> READY (temporizador/evento)
     *                           -> EXECUTE -> HALT (fin normal)
     *                           -> EXECUTE -> DEAD (error fatal)
     */
    typedef enum vm_state {
        /**
         * @brief El proceso esta listo para ejecutarse pero no tiene la CPU virtual.
         *
         * Condiciones de entrada:
         *   - El proceso fue creado y su contexto esta preparado.
         *   - El proceso cedio la CPU (yield) voluntariamente.
         *   - El proceso fue desbloqueado tras un evento o temporizador.
         *
         * Desde READY el scheduler puede seleccionar el proceso para ejecutarlo.
         */
        READY,

        /**
         * @brief El proceso tiene actualmente la CPU virtual asignada.
         *
         * Condiciones de entrada:
         *   - El scheduler selecciono este proceso en su quantum.
         *
         * El proceso puede cambiar a DECODE, WAIT_IO, BLOCKED, HALT o DEAD
         * segun el resultado de la instruccion en curso.
         */
        RUNNING,

        /**
         * @brief El proceso esta bloqueado esperando un evento, tiempo o senal externa.
         *
         * Condiciones de entrada:
         *   - El proceso espera un evento de sincronizacion.
         *   - El proceso espera un temporizador (time_sleep).
         *
         * El proceso no sera ejecutado hasta que sea desbloqueado por el evento esperado.
         */
        BLOCKED,

        /**
         * @brief El proceso ha terminado su ejecucion de forma permanente por error fatal.
         *
         * Condiciones de entrada:
         *   - La funcion principal del proceso retorno.
         *   - El proceso encontro un error irrecuperable (SEGFAULT, instruccion ilegal, etc.).
         *   - El proceso fue terminado explicitamente.
         *
         * Un proceso DEAD no volvera a ejecutarse; sus recursos pueden liberarse.
         */
        DEAD,

        /**
         * @brief El scheduler esta descodificando la instruccion actual del proceso.
         *
         * Condiciones de entrada:
         *   - El proceso paso de RUNNING/READY a la fase de descodificacion.
         *
         * En este estado se determina el opcode, los operandos y el modo de
         * direccionamiento de la instruccion en PC.
         */
        DECODE,

        /**
         * @brief El scheduler esta ejecutando la instruccion ya descodificada.
         *
         * Condiciones de entrada:
         *   - La fase DECODE completo con exito (EVT_DECODE_DONE).
         *
         * La ejecucion puede modificar registros, memoria o el estado interno
         * del proceso, y puede generar los eventos EVT_EXEC_DONE, EVT_IO_WAIT,
         * EVT_HALT o EVT_ERROR.
         */
        EXECUTE,

        /**
         * @brief El proceso esta esperando la finalizacion de una operacion de E/S.
         *
         * Condiciones de entrada:
         *   - La instruccion en EXECUTE detecto que necesita esperar un dispositivo externo.
         *
         * El scheduler puede ejecutar otros procesos mientras este espera.
         * Cuando la E/S termina se genera EVT_IO_READY y el proceso vuelve a READY.
         */
        WAIT_IO,

        /**
         * @brief El proceso detendio su ejecucion por una instruccion HALT o parada explicita.
         *
         * Condiciones de entrada:
         *   - La instruccion HALT fue encontrada.
         *   - El sistema solicito la parada del proceso.
         *
         * Un proceso HALT no ejecutara mas instrucciones pero puede ser reiniciado.
         */
        HALT,

        /**
         * @brief El proceso fue recien creado y no esta listo para ejecutarse.
         *
         * Permanece en este estado hasta que se llama a make_ready(pid),
         * que lo pone en READY y lo inserta en la cola del scheduler.
         */
        NEW,

        /**
         * @brief Centinela que indica el numero total de estados.
         *
         * No es un estado real; se usa para dimensionar la tabla de transiciones
         * fsm[NUM_STATES][NUM_EVENTS].
         */
        NUM_STATES
    } vm_state;

    /**
     * @brief Eventos que disparan transiciones en la FSM del scheduler.
     *
     * Cada evento representa un suceso interno del ciclo de instruccion o
     * una condicion externa que obliga a cambiar el estado del proceso.
     */
    enum vm_event {
        /**
         * @brief El scheduler selecciono este proceso para ejecutarse.
         *
         * Generado por el planificador cuando decide asignar la CPU virtual al proceso.
         * Transicion habitual: READY -> RUNNING.
         */
        EVT_SCHEDULED,

        /**
         * @brief El proceso cede voluntariamente la CPU al scheduler.
         *
         * Permite que otros procesos tengan turno sin que el proceso actual haya
         * agotado sus reducciones.
         */
        EVT_YIELD,

        /**
         * @brief La instruccion fue descodificada correctamente.
         *
         * Se dispara al final de la fase DECODE cuando el opcode, operandos
         * y modo de direccionamiento ya estan disponibles en decoded_ptr.
         * Transicion habitual: DECODE -> EXECUTE.
         */
        EVT_DECODE_DONE,

        /**
         * @brief La instruccion se ejecuto completamente sin bloquear.
         *
         * Se dispara al final de la fase EXECUTE cuando no se requiere E/S
         * ni sincronizacion adicional.
         * Transicion habitual: EXECUTE -> DECODE (siguiente instruccion).
         */
        EVT_EXEC_DONE,

        /**
         * @brief La instruccion necesita esperar una operacion de E/S.
         *
         * Se dispara dentro de EXECUTE cuando la instruccion detecta que
         * debe esperar un dispositivo o evento externo.
         * Transicion habitual: EXECUTE -> WAIT_IO.
         */
        EVT_IO_WAIT,

        /**
         * @brief La operacion de E/S ha finalizado y el proceso puede continuar.
         *
         * Generado por el subsistema de E/S cuando la operacion esperada termino.
         * Transicion habitual: WAIT_IO -> READY.
         */
        EVT_IO_READY,

        /**
         * @brief Se encontro una instruccion HALT o se solicito detener el proceso.
         *
         * Generado por la instruccion HALT o por una parada explicita del sistema.
         * Transicion habitual: EXECUTE -> HALT.
         */
        EVT_HALT,

        /**
         * @brief Ocurrio un error fatal durante la ejecucion del proceso.
         *
         * Se dispara cuando se detecta un error irrecuperable (acceso de memoria invalido,
         * instruccion desconocida, division por cero, etc.).
         * Transicion habitual: cualquier estado -> DEAD.
         */
        EVT_ERROR,

        /**
         * @brief Centinela que indica el numero total de eventos.
         *
         * No es un evento real; se usa para dimensionar la tabla de transiciones
         * fsm[NUM_STATES][NUM_EVENTS].
         */
        NUM_EVENTS
    };

    /**
     * @brief Devuelve el nombre legible de un evento de la FSM.
     *
     * Util para mensajes de traza y depuracion.
     *
     * @param e Evento de la FSM.
     * @return  Cadena con el nombre del evento, o "UNKNOWN_EVENT" si no se reconoce.
     */
    static const char *event_name(vm_event e) {
        switch (e) {
            case EVT_DECODE_DONE: return "EVT_DECODE_DONE"; // descodificacion completada
            case EVT_EXEC_DONE:   return "EVT_EXEC_DONE";   // ejecucion completada
            case EVT_IO_WAIT:     return "EVT_IO_WAIT";     // esperando E/S
            case EVT_IO_READY:    return "EVT_IO_READY";    // E/S lista
            case EVT_HALT:        return "EVT_HALT";         // instruccion HALT
            case EVT_ERROR:       return "EVT_ERROR";        // error fatal
            case EVT_SCHEDULED:   return "EVT_SCHEDULED";   // proceso seleccionado
            default:              return "UNKNOWN_EVENT";    // evento desconocido
        }
    }

    /**
     * @brief Devuelve el nombre legible de un estado de la FSM.
     *
     * @param state Estado de la FSM.
     * @return      Cadena con el nombre del estado, o "UNKNOWN" si no se reconoce.
     */
    static const char *vm_state_to_str(vm_state state) {
        switch (state) {
            case READY:    return "READY";    // listo para ejecutarse
            case RUNNING:  return "RUNNING";  // ejecutandose
            case BLOCKED:  return "BLOCKED";  // bloqueado esperando evento
            case DECODE:   return "DECODE";   // descodificando instruccion
            case EXECUTE:  return "EXECUTE";  // ejecutando instruccion
            case WAIT_IO:  return "WAIT_IO";  // esperando operacion de E/S
            case HALT:     return "HALT";     // detenido por HALT
            case DEAD:     return "DEAD";     // terminado permanentemente
            case NEW:      return "NEW";      // recien creado, no listo
            default:       return "UNKNOWN";  // estado desconocido
        }
    }

    /**
     * @brief Entrada de la tabla de transiciones de la FSM.
     *
     * Cada celda fsm[estado][evento] contiene el estado siguiente y la accion
     * opcional que debe ejecutarse al dispararse el evento.
     */
    typedef struct Transition {
        vm_state next;             ///< Estado al que debe pasar el proceso tras el evento

        void (*action)(ProcessVM *); ///< Accion a ejecutar durante la transicion (puede ser nullptr)

        /**
         * @brief Construye una transicion con el estado y la accion indicados.
         *
         * @param n Estado siguiente (por defecto READY).
         * @param a Accion a ejecutar durante la transicion (por defecto nullptr).
         */
        Transition(vm_state n = READY, void (*a)(ProcessVM *) = nullptr)
            : next(n), action(a) {}
    } Transition;

} // namespace runtime

#endif // VM_STATE_EVENT_H
