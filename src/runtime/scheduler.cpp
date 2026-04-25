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
 * @file scheduler.cpp
 * @brief Implementacion del planificador de procesos de VestaVM.
 *
 * Implementa @c Scheduler: asignacion de procesos a hilos nativos, bucle
 * principal de planificacion con balance de carga, ganchos de temporalizacion
 * y transiciones de estado de proceso (READY -> RUNNING -> BLOCKED/DEAD).
 */#include "runtime/scheduler.h"

#include "runtime/decode_instruction.h"
#include "distrib/dist_runtime.h"
#include "distrib/dist_debug.h"
#include "runtime/runtime.h"

namespace runtime {

    /**
     * @brief Construye el scheduler, lo asocia a la VM indicada e inicializa la FSM.
     *
     * @param id_scheduler Identificador unico del scheduler dentro de su VM.
     * @param vm_reference Referencia a la instancia VM propietaria.
     */
    Scheduler::Scheduler(uint32_t id_scheduler, VM &vm_reference)
        : id_scheduler(id_scheduler), vm_reference(vm_reference) {
        init_fsm(); // inicializar la tabla de transiciones antes de aceptar procesos
    }

    /**
     * @brief Ejecuta un unico paso de la FSM para el proceso activo (instance).
     *
     * Usa "computed goto" para despachar al bloque de codigo del estado actual
     * sin penalizar el predictor de ramas con un switch.  Ejecuta exactamente
     * una transicion y retorna al scheduler.
     *
     * Estados y su comportamiento:
     *   - READY    -> genera EVT_SCHEDULED y salta a RUNNING.
     *   - RUNNING  -> genera EVT_SCHEDULED y salta a DECODE.
     *   - DECODE   -> genera EVT_DECODE_DONE (descodifica) y salta a EXECUTE.
     *   - EXECUTE  -> ejecuta la instruccion y retorna.
     *   - BLOCKED  -> retorna sin hacer nada (espera evento externo).
     *   - WAIT_IO  -> retorna sin hacer nada (espera E/S).
     *   - HALT     -> retorna inmediatamente.
     *   - DEAD     -> retorna inmediatamente.
     *   - NEW      -> retorna sin hacer nada.
     *
     * @param process Proceso sobre el que se ejecutara el paso de la FSM.
     */
    void Scheduler::run_fsm_step(ProcessVM *process) {
        // deshabilitar advertencias de GNU sobre "computed goto" (extension de GCC)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpedantic"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

        /**
         * Tabla de despacho por estado.  Cada entrada es la etiqueta de destino
         * del goto calculado.  El orden debe coincidir exactamente con el enum vm_state.
         * Referencia sobre prediccion de ramas en emuladores:
         *   https://stackoverflow.com/questions/11668090/how-to-deal-with-branch-prediction-when-using-a-switch-case-in-cpu-emulation
         */
        static void *dispatch_table[NUM_STATES] = {
            &&READY_LABEL,    // READY
            &&RUNNING_LABEL,  // RUNNING
            &&BLOCKED_LABEL,  // BLOCKED
            &&DEAD_LABEL,     // DEAD
            &&DECODE_LABEL,   // DECODE
            &&EXECUTE_LABEL,  // EXECUTE
            &&WAIT_IO_LABEL,  // WAIT_IO
            &&HALT_LABEL,     // HALT
            &&NEW_LABEL       // NEW
        };

        goto *dispatch_table[instance->state]; // saltar al bloque del estado actual

    READY_LABEL:
        on_event(EVT_SCHEDULED); // READY -> RUNNING
        goto *dispatch_table[instance->state]; // redispachar al nuevo estado

    RUNNING_LABEL:
        on_event(EVT_SCHEDULED); // RUNNING -> DECODE
        goto *dispatch_table[instance->state]; // redispachar al nuevo estado

    BLOCKED_LABEL:
        return; // esperar evento externo, no hay nada que hacer

    DECODE_LABEL:
        on_event(EVT_DECODE_DONE); // descodificar la instruccion y pasar a EXECUTE
        // una sola llamada cubre decode+execute; 1 llamada = 1 instruccion = 1 reduccion
        goto *dispatch_table[instance->state];

    EXECUTE_LABEL:
        // ejecutar la instruccion; si es bloqueante retorna EVT_IO_WAIT, si no EVT_EXEC_DONE
        on_event(execute_instruction(process));
        return;

    WAIT_IO_LABEL:
        return; // esperar finalizacion de E/S; el manejador de E/S llamara on_event(EVT_IO_READY)

    HALT_LABEL:
    DEAD_LABEL:
        return; // estado terminal; el scheduler no volvera a llamar a este metodo

    NEW_LABEL:
        return; // proceso recien creado; no ejecutar hasta que make_ready() lo active

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    }

    /**
     * @brief Inicializa la tabla de transiciones de la FSM (fsm[][]).
     *
     * Rellena la tabla con el estado por defecto (sin cambio de estado, sin accion)
     * y luego sobreescribe las transiciones validas:
     *
     *   READY  -> EVT_SCHEDULED   -> RUNNING
     *   RUNNING-> EVT_SCHEDULED   -> DECODE
     *   DECODE -> EVT_DECODE_DONE -> EXECUTE  (accion: descodificar instruccion)
     *   EXECUTE-> EVT_EXEC_DONE   -> DECODE
     *   EXECUTE-> EVT_IO_WAIT     -> WAIT_IO
     *   WAIT_IO-> EVT_IO_READY    -> READY
     *   EXECUTE-> EVT_HALT        -> HALT
     *   *      -> EVT_ERROR       -> DEAD
     *   *      -> EVT_YIELD       -> READY
     *   NEW    -> EVT_SCHEDULED   -> READY
     */
    void Scheduler::init_fsm() {
        // inicializar toda la tabla con "sin cambio de estado, sin accion"
        for (uint64_t s = 0; s < NUM_STATES; s++) {
            for (uint64_t e = 0; e < NUM_EVENTS; e++) {
                fsm[s][e].next   = (vm_state) s; // por defecto el estado no cambia
                fsm[s][e].action = nullptr;       // sin accion por defecto
            }
        }

        // READY -> RUNNING al ser seleccionado por el scheduler
        fsm[READY][EVT_SCHEDULED] = {RUNNING, nullptr};

        // RUNNING -> DECODE: comenzar el ciclo de instruccion
        fsm[RUNNING][EVT_SCHEDULED] = {DECODE, nullptr};

        // DECODE -> EXECUTE: descodificar la instruccion en PC
        fsm[DECODE][EVT_DECODE_DONE] = Transition{
            EXECUTE,
            [](ProcessVM *vm) {
                decode_instruction(vm); // descodificar la instruccion apuntada por PC
            }
        };

        // EXECUTE -> DECODE: instruccion completada sin bloqueos
        fsm[EXECUTE][EVT_EXEC_DONE] = Transition{
            DECODE,
            [](ProcessVM *vm) {
                // la ejecucion avanza el PC en run_loop; aqui no se necesita accion
            }
        };

        // EXECUTE -> WAIT_IO: instruccion bloqueante, esperar finalizacion de E/S
        fsm[EXECUTE][EVT_IO_WAIT] = Transition{
            WAIT_IO,
            [](ProcessVM *vm) {
                // el subsistema de E/S notificara con EVT_IO_READY cuando termine
            }
        };

        // WAIT_IO -> READY: la operacion de E/S termino, volver a planificar
        fsm[WAIT_IO][EVT_IO_READY] = Transition{
            READY,
            [](ProcessVM *vm) {
                // el proceso puede continuar en el proximo quantum
            }
        };

        // EXECUTE -> HALT: instruccion HALT encontrada
        fsm[EXECUTE][EVT_HALT] = Transition{
            HALT,
            [](ProcessVM *vm) {
                // la VM no ejecutara mas instrucciones hasta ser reiniciada
            }
        };

        // EVT_YIELD: ceder la CPU voluntariamente desde cualquier fase de ejecucion
        fsm[EXECUTE][EVT_YIELD] = {READY, nullptr}; // fin de quantum en EXECUTE -> READY
        fsm[DECODE][EVT_YIELD]  = {READY, nullptr}; // fin de quantum en DECODE  -> READY
        fsm[RUNNING][EVT_YIELD] = {READY, nullptr}; // fin de quantum en RUNNING -> READY

        // NEW -> READY al ser activado por make_ready()
        fsm[NEW][EVT_SCHEDULED] = {READY, nullptr};

        // cualquier estado -> DEAD al producirse un error fatal
        for (int s = 0; s < NUM_STATES; s++) {
            fsm[s][EVT_ERROR] = Transition{
                DEAD,
                [](ProcessVM *vm) {
                    // el proceso sera eliminado por el scheduler; no hay accion de recuperacion
                }
            };
        }
    }

    /**
     * @brief Indica si hay al menos un proceso vivo en este scheduler.
     * @return true si alive_count > 0.
     */
    bool Scheduler::has_alive_processes() const {
        return alive_count > 0; // consulta O(1) del contador atomico
    }

    /**
     * @brief Destructor: marca todos los procesos como DEAD y libera recursos.
     *
     * Pone la bandera de parada y marca la instruccion activa como bloqueante
     * para evitar que el scheduler intente ejecutar instrucciones durante la
     * destruccion.
     */
    Scheduler::~Scheduler() {
        should_kill = true; // indicar al run_loop que debe terminar

        for (auto &p: processes) {
            if (p->decoded_ptr != nullptr) {
                // marcar la instruccion activa como bloqueante para interrumpir ejecucion
                p->decoded_ptr->flags_info.blocking = true;
                p->state = DEAD; // transicionar a estado terminal
            }
        }
    }

    /**
     * @brief Bucle principal del planificador de procesos.
     *
     * Implementa el ciclo de planificacion cooperativa/preemptiva:
     *   1. Obtiene el proximo proceso listo (schedule_next).
     *   2. Si no hay procesos listos:
     *        - Si no hay procesos vivos en toda la VM, la detiene y sale.
     *        - Si hay procesos vivos en otros schedulers, duerme en el semaforo.
     *   3. En fast-path (sin hooks): decode + execute directo sin FSM completa.
     *   4. En slow-path (con hooks): run_fsm_step completo con todos los eventos.
     *   5. Al agotar reducciones: genera EVT_YIELD y reencola el proceso en READY.
     *
     * @note Este metodo debe ejecutarse en un hilo dedicado del ThreadPool.
     */
    void Scheduler::run_loop() {
        // continuar mientras no se solicite parada y la VM siga activa
        while (!should_kill && vm_reference.vm_running) {
            instance = schedule_next(); // obtener el proximo proceso listo

            if (!instance) {
                // no hay procesos listos; verificar si quedan procesos vivos en toda la VM
                if (!vm_reference.has_alive_processes()) {
                    if (!vm_reference.vm_persistent) {
                        vm_reference.vm_running = false; // ninguna VM tiene procesos vivos: detener
                        break;                           // terminar este scheduler
                    }
                    // modo persistente (servidor distribuido): esperar a que llegue un proceso
                }

                // hay procesos vivos en otros schedulers (o modo persistente); dormir hasta que haya trabajo
                is_waiting = true;
                sem.acquire(); // bloquear hasta que make_ready() o stop() libere el semaforo
                is_waiting = false;

                // verificar si nos despertaron porque la VM debe detenerse
                if (should_kill || !vm_reference.vm_running)
                    break;

                // intentar obtener trabajo tras despertar
                instance = schedule_next();
                continue;
            }

            if (!has_hooks) {
                // === FAST PATH: decode + execute directo sin FSM completa ni hooks ===
                // READY/RUNNING no tienen accion; saltar directamente a DECODE
                if (instance->state == READY || instance->state == RUNNING)
                    instance->state = DECODE;

                // traza de primera ejecucion del proceso para depuracion distribuida
                if (instance->tsc == 0) {
                    DIST_DBG("SCHED %u: primera ejecucion PID=(sched=%u local=%llu) "
                             "PC=0x%llX rspawn_fid=%llu estado=%d",
                             id_scheduler,
                             instance->pid.scheduler_id,
                             (unsigned long long)instance->pid.local_pid,
                             (unsigned long long)instance->registers.rip.raw(),
                             (unsigned long long)instance->rspawn_future_id,
                             (int)instance->state);
                }

                while (instance->reductions_remaining > 0) {
                    decode_instruction(instance); // descodificar la instruccion en PC

                    // poner en EXECUTE para que las instrucciones de control de flujo (HLT, etc.)
                    // encuentren el estado correcto en sus transiciones internas de on_event
                    instance->state = EXECUTE;
                    vm_event evt = execute_instruction(instance); // ejecutar la instruccion
                    instance->reductions_remaining--;

                    // camino rapido: instruccion normal completada -> continuar con la siguiente
                    if (__builtin_expect(evt == EVT_EXEC_DONE, 1)) {
                        instance->state = DECODE; // preparar para la siguiente descodificacion
                        continue;
                    }

                    // la instruccion cambio el estado de forma autonoma (ej. HLT via on_event)
                    if (instance->state == HALT || instance->state == DEAD) {
                        alive_count--; // decrementar el contador de procesos vivos
                        DIST_DBG("SCHED %u: proceso PID=(sched=%u local=%llu) -> %s  "
                                 "r0=%llu err=%d tsc=%llu PC=0x%llX",
                                 id_scheduler,
                                 instance->pid.scheduler_id,
                                 (unsigned long long)instance->pid.local_pid,
                                 instance->state == HALT ? "HALT" : "DEAD",
                                 (unsigned long long)instance->registers.regs[0].qword(),
                                 (int)instance->err_thread,
                                 (unsigned long long)instance->tsc,
                                 (unsigned long long)instance->registers.rip.raw());
                        // si el proceso fue creado por rspawn remoto, notificar al nodo origen con r0
                        if (instance->rspawn_future_id != 0 &&
                            instance->rspawn_origin_node != 0xFFFFFFFFu &&
                            vm_reference.dist_runtime) {
                            vm_reference.dist_runtime->notify_rspawn_halt(instance);
                        }
                        instance = nullptr;
                        break;
                    }

                    // E/S genuina: poner en WAIT_IO y dejar de procesar este proceso
                    DIST_DBG("SCHED %u: proceso PID=(sched=%u local=%llu) -> WAIT_IO "
                             "tsc=%llu PC=0x%llX err=%d",
                             id_scheduler,
                             instance->pid.scheduler_id,
                             (unsigned long long)instance->pid.local_pid,
                             (unsigned long long)instance->tsc,
                             (unsigned long long)instance->registers.rip.raw(),
                             (int)instance->err_thread);
                    instance->state = WAIT_IO;
                    instance = nullptr;
                    break;
                }
            } else {
                // === SLOW PATH: FSM completo con hooks de depuracion ===
                while (instance->reductions_remaining > 0) {
                    run_fsm_step(instance); // ejecutar un paso de la FSM
                    instance->reductions_remaining--;

                    // comprobar si el proceso alcanzo un estado terminal o bloqueante
                    if (instance->state == DEAD || instance->state == HALT) {
                        alive_count--; // decrementar el contador de procesos vivos
                        DIST_DBG("SCHED %u (slow): proceso PID=(sched=%u local=%llu) -> %s  "
                                 "r0=%llu err=%d tsc=%llu PC=0x%llX",
                                 id_scheduler,
                                 instance->pid.scheduler_id,
                                 (unsigned long long)instance->pid.local_pid,
                                 instance->state == HALT ? "HALT" : "DEAD",
                                 (unsigned long long)instance->registers.regs[0].qword(),
                                 (int)instance->err_thread,
                                 (unsigned long long)instance->tsc,
                                 (unsigned long long)instance->registers.rip.raw());
                        // notificar al nodo origen si el proceso vino de un rspawn remoto
                        if (instance->rspawn_future_id != 0 &&
                            instance->rspawn_origin_node != 0xFFFFFFFFu &&
                            vm_reference.dist_runtime) {
                            vm_reference.dist_runtime->notify_rspawn_halt(instance);
                        }
                        instance = nullptr;
                        break;
                    }
                    if (instance->state == WAIT_IO || instance->state == BLOCKED) {
                        instance = nullptr; // el proceso espera un evento externo
                        break;
                    }
                }
            }

            // si instance es nulo el proceso ya fue gestionado en el bloque anterior
            if (instance == nullptr) {
                continue;
            }

            // el proceso agoto sus reducciones: ceder la CPU y reencolar en READY
            if (instance->reductions_remaining == 0) {
                on_event(EVT_YIELD); // transicion a READY por fin de quantum
            }

            // reencolar el proceso si sigue en estado ejecutable
            if (instance->state == READY) {
                std::lock_guard<std::mutex> lock(queue_mutex);
                ready_queue.push_back(instance); // volver a poner al final de la cola FIFO
            }
        }
    }

    /**
     * @brief Genera una representacion textual del scheduler para depuracion.
     * @return Cadena con el ID, estado, contadores de procesos y tiempos.
     */
    std::string Scheduler::to_string() const {
        std::ostringstream ss;

        ss << "Scheduler[" << id_scheduler << "] {\n"
           << "  waiting: "                << (is_waiting       ? "yes" : "no") << "\n"
           << "  should_kill: "            << (should_kill       ? "yes" : "no") << "\n"
           << "  profiler_running: "       << (profiler_running  ? "yes" : "no") << "\n"
           << "  processes_total: "        << processes.size()    << "\n"
           << "  ready_queue: "            << ready_queue.size()  << "\n"
           << "  alive_processes: "        << vm_reference.has_alive_processes() << "\n"
           << "  reductions_now: "         << reductions_now      << "\n"
           << "  profiler_instr_counter: " << profiler_instr_counter << "\n"
           << "  time_exec(ns): "          << time_exec   << "\n"
           << "  time_decode(ns): "        << time_decode << "\n"
           << "  time_event(ns): "         << time_event  << "\n"
           << "}";

        return ss.str();
    }

    /**
     * @brief Reinicia el estado interno del scheduler sin destruirlo.
     *
     * Limpia colas, indices, procesos, contadores y banderas para que el
     * scheduler pueda reutilizarse en un nuevo ciclo de ejecucion.
     */
    void Scheduler::reset() {
        // limpiar estructuras de datos del scheduler
        ready_queue.clear(); // vaciar la cola de procesos listos
        pid_index.clear();   // limpiar el indice PID -> proceso
        processes.clear();   // liberar todos los procesos (unique_ptr los destruye)

        // reiniciar contadores de ejecucion
        next_pid               = 0; // reiniciar generador de PIDs locales
        reductions_now         = 0; // reiniciar contador de reducciones del quantum actual
        profiler_instr_counter = 0; // reiniciar muestras del profiler
        time_exec              = 0; // reiniciar tiempo de ejecucion acumulado
        time_decode            = 0; // reiniciar tiempo de descodificacion acumulado
        time_event             = 0; // reiniciar tiempo de transiciones acumulado

        // reiniciar banderas de control
        is_waiting       = false; // no estamos esperando en el semaforo
        should_kill      = false; // no hay solicitud de parada
        profiler_running = false; // profiler desactivado hasta nueva configuracion
        alive_count      = 0;     // ningun proceso vivo

        instance = nullptr; // ninguna instancia activa
    }

    /**
     * @brief Extrae el proximo proceso de la cola FIFO de procesos listos.
     *
     * Reinicia el contador de reducciones del proceso seleccionado al valor
     * por defecto y actualiza el puntero instance.
     *
     * @return Puntero al proximo proceso listo, o nullptr si la cola esta vacia.
     */
    ProcessVM *Scheduler::schedule_next() {
        std::lock_guard<std::mutex> lock(queue_mutex); // proteger el acceso concurrente a ready_queue
        if (ready_queue.empty())
            return nullptr; // no hay procesos listos en la cola

        ProcessVM *p = ready_queue.front(); // obtener el proceso al frente de la cola FIFO
        ready_queue.pop_front();            // eliminarlo de la cola

        p->reductions_remaining = reductions_remaining_default; // reiniciar el quantum del proceso
        instance = p; // actualizar el puntero de instancia activa
        return p;
    }

    /**
     * @brief Crea un nuevo proceso virtual en este scheduler.
     *
     * Asigna un PID local, construye el ProcessVM, lo registra en el indice
     * de PIDs y devuelve su GlobalPID.  El proceso nace en estado NEW.
     *
     * @return PID global del proceso recien creado.
     */
    GlobalPID Scheduler::spawn() {
        GlobalPID pid = {id_scheduler, next_pid++}; // construir PID global con nuevo ID local

        auto       proc = std::make_unique<ProcessVM>(*this, pid); // crear el proceso
        ProcessVM *raw  = proc.get();                              // guardar raw pointer antes de mover

        processes.push_back(std::move(proc)); // transferir propiedad al vector
        pid_index[pid] = raw;                 // registrar en el indice para busqueda O(1)

        return pid;
    }

    /**
     * @brief Pone el proceso con el PID indicado en estado READY y lo encola.
     *
     * Si el proceso estaba en NEW, HALT o DEAD se incrementa alive_count.
     * Las llamadas posteriores (p.ej. tras WAIT_IO) no modifican el contador.
     *
     * @param pid PID global del proceso que debe pasar a estado READY.
     */
    void Scheduler::make_ready(GlobalPID pid) {
        ProcessVM *p = pid_index[pid]; // buscar el proceso por PID en O(1)

        // incrementar alive_count solo la primera vez o al reactivar un proceso terminado
        if (p->state == NEW || p->state == HALT || p->state == DEAD)
            alive_count++; // el proceso pasa a contar como "vivo"

        p->state = READY; // marcar como listo para ejecutarse

        {
            std::lock_guard<std::mutex> lock(queue_mutex); // proteger la cola ante accesos concurrentes
            ready_queue.push_back(p); // insertar al final de la cola FIFO
        }
    }

    /**
     * @brief Elimina el proceso con el PID indicado del scheduler.
     *
     * Decrementa alive_count si el proceso era "vivo", elimina su entrada del
     * indice y lo borra del vector con la tecnica swap-and-pop O(1).
     *
     * @param pid PID global del proceso a eliminar.
     */
    void Scheduler::kill(GlobalPID pid) {
        auto it = pid_index.find(pid); // buscar por PID en el indice
        if (it == pid_index.end()) return; // PID no encontrado; nada que hacer

        ProcessVM *p = it->second;

        // decrementar alive_count solo si el proceso era "vivo"
        if (p->state != NEW && p->state != DEAD && p->state != HALT)
            alive_count--;

        pid_index.erase(it); // eliminar del indice de PIDs

        // swap-and-pop O(1): mover el ultimo elemento al hueco y eliminar el ultimo
        for (size_t i = 0; i < processes.size(); i++) {
            if (processes[i].get() == p) {
                if (i != processes.size() - 1)
                    processes[i] = std::move(processes.back()); // llenar el hueco con el ultimo
                processes.pop_back(); // eliminar el ultimo (que era el proceso a borrar)
                break;
            }
        }
    }

    /**
     * @brief Registra un hook de depuracion sin sincronizacion (solo antes del inicio).
     *
     * @param hook Callback de depuracion a anadir.
     */
    void Scheduler::free_add_debug_hook(DebugHook hook) {
        debug_hooks.push_back(hook); // anadir al vector de hooks sin sincronizacion
        has_hooks = true;            // activar el slow path en run_loop
    }

    /**
     * @brief Registra un hook de depuracion de forma thread-safe.
     *
     * Actualmente delega en free_add_debug_hook(); anadir mutex si se necesita
     * registrar hooks mientras el scheduler ya esta en ejecucion.
     *
     * @param hook Callback de depuracion a anadir.
     */
    void Scheduler::add_debug_hook(DebugHook hook) {
        free_add_debug_hook(hook); // delegar en la version sin sincronizacion
    }

#ifdef PROFILE_FAST
    // vm_hook sustituida por macro vacia cuando PROFILE_FAST esta definido
#else
    /**
     * @brief Invoca todos los hooks de depuracion del scheduler del proceso.
     *
     * Si el scheduler no tiene hooks activos (has_hooks=false) retorna inmediatamente
     * sin iterarlos para minimizar la penalizacion en el hot path.
     *
     * @param process Proceso cuyo scheduler contiene los hooks.
     * @param stage   Fase del pipeline en la que se disparan los hooks.
     */
    void vm_hook(ProcessVM *process, DebugStage stage) {
        if (!process->scheduler.has_hooks) return; // salida rapida si no hay hooks registrados

        for (auto &hook : process->scheduler.debug_hooks)
            hook(process, stage); // invocar cada hook con el proceso y la fase actual
    }
#endif

    /**
     * @brief Ejecuta la transicion de la FSM correspondiente al evento @p e.
     *
     * Consulta fsm[instance->state][e], ejecuta la accion asociada (si existe)
     * y actualiza el estado del proceso al estado siguiente de la tabla.
     *
     * @param e Evento que desencadena la transicion.
     */
    void Scheduler::on_event(vm_event e) {
        const Transition &t = fsm[instance->state][e]; // obtener la transicion de la tabla

        if (t.action)
            t.action(instance); // ejecutar la accion de la transicion si existe

        instance->state = t.next; // cambiar el proceso al estado siguiente
    }

    /**
     * @brief Hilo de perfilado continuo para un scheduler.
     *
     * Duerme un segundo, calcula IPS (instrucciones por segundo) y el porcentaje
     * de CPU consumido por la fase de ejecucion, e imprime los resultados con
     * vesta::scout() (thread-safe).
     *
     * Se detiene cuando should_kill, !profiler_running o !vm_running sean true,
     * o cuando el scheduler este esperando (is_waiting=true).
     *
     * @param scheduler Puntero al scheduler que se va a perfilar.
     */
    void profiler_thread(Scheduler *scheduler) {
        uint64_t last_instr = 0; // ultimo valor del contador de instrucciones leido
        uint64_t last_exec  = 0; // ultimo valor del tiempo de ejecucion leido (ns)

        while (scheduler->profiler_running &&
               !scheduler->should_kill &&
               scheduler->vm_reference.vm_running &&
               scheduler->is_waiting != true) {

            std::this_thread::sleep_for(std::chrono::seconds(1)); // muestrear cada segundo

            // calcular delta de instrucciones en el ultimo segundo
            uint64_t now_instr   = scheduler->profiler_instr_counter;
            uint64_t delta_instr = now_instr - last_instr; // variacion desde la ultima muestra
            last_instr           = now_instr;              // actualizar el ultimo valor

            // IPS: profiler_instr_counter se incrementa cada 256 instrucciones
            uint64_t ips = delta_instr;

            // calcular porcentaje de CPU usando el tiempo de ejecucion acumulado
            uint64_t now_exec   = scheduler->time_exec;
            uint64_t delta_exec = now_exec - last_exec; // nanosegundos ocupados en el ultimo segundo
            last_exec           = now_exec;

            double cpu = ((double(delta_exec) / 1e9) * 100.0); // convertir ns a porcentaje de segundo
            if (cpu > 100.0) cpu = 100.0;                       // saturar al maximo fisico posible

            // imprimir estadisticas usando la salida thread-safe
            vesta::scout()
                << "[scheduler " << scheduler->id_scheduler << "] "
                << "IPS=" << ips
                << " | CPU=" << cpu << "%\n";
        }
    }

} // namespace runtime
