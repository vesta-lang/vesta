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
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <queue>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>

#include "runtime/runtime.h"
#include "runtime/proceso_runtime.h"
#include "vm_state_event.h"
#include "profiler/timer.h"

#include "runtime/pid.h"

namespace runtime {
    class ProcessVM;
    struct Transition;
    class VM;

    // Polyfill de std::counting_semaphore para GCC < 11.
    // Drop-in: reemplazar con std::counting_semaphore<> al actualizar el compilador.
    struct BinarySemaphore {
        void release() {
            std::lock_guard lock(mtx_);
            ++count_;
            cv_.notify_one();
        }
        void acquire() {
            std::unique_lock lock(mtx_);
            cv_.wait(lock, [&]{ return count_ > 0; });
            --count_;
        }
    private:
        std::mutex mtx_;
        std::condition_variable cv_;
        int count_ = 0;
    };

    /**
     * Estados de depuracion
     */
    enum class DebugStage {
        /**
         * realizar la depuracion en fase de descoficacion de instruccion.
         *
         */
        DecodeBegin,
        DecodeEnd,

        /**
         * permite realiza la depuracion en la fase previa a la ejecuccion de la
         * instruccion.
         */
        ExecuteBegin,

        /**
         * permite realizar la depuracion en la fase final de la ejecuccion de la instruccion.
         */
        ExecuteEnd,

        /**
         * Permite hacer el hook antes de hacer el cambio de un estado a taves de un
         * evento
         */
        OnEventBegin,

        /**
         * Permite hacer el hook despues del cambio de estado y el desencadenamiento del
         * evento.
         */
        OnEventEnd,
    };

    using DebugHook = void(*)(ProcessVM *vm, DebugStage stage);

    class Scheduler {
    public:
        /**
         * ID del gestor de procesos, un gestor de procesos, es un hilo nativo, no se permite tener mas de
         * 2**32 gestores de procesos.
         * **Es asignado por la VM**.
         */
        const uint32_t id_scheduler;

        /**
         * Se usa para realizar depuracion o realizar ciertas acciones antes
         * de una fase, especificada
         */
        std::vector<DebugHook> debug_hooks;

        /**
         * Contador PID de procesos.
         */
        uint64_t next_pid = 0;

        /**
         * Apunta a un proceso en actual ejecuccion de la tabla process_vm
         */
        ProcessVM *instance = nullptr;

        /**
         * Contador de instrucciones ejecutadas actualmente por el proceso en ejecuccion, cuando se alcanze
         * las "reducciones" indicadas por el proceso, se procede con el siguiente.
         */
        uint64_t reductions_now = 0;

        /**
         * Contiene todos los procesos de la instancia virtual.
         *
         *  ¿Por qué unique_ptr en el vector de procesos?
         *      - La VM es la dueña de los procesos
         *
         * Los procesos nacen y mueren dentro de la VM.
         * No deben tener múltiples dueños.
         * - unique_ptr deja claro quién es el propietario
         *     La VM crea el proceso
         *     La VM lo destruye
         *     Nadie más puede borrarlo accidentalmente
         * - Evita fugas de memoria
         *
         * - Cuando un proceso muere, simplemente:
         *      processes.erase(it);
         *
         * Y el unique_ptr libera todo_.
         */
        std::vector<std::unique_ptr<ProcessVM> > processes;

        /**
         * Tabla de procesos con su correspondiente PID.
         */
        std::unordered_map<GlobalPID, ProcessVM *> pid_index;

        /**
         * Tabla de transiciones
         */
        Transition fsm[NUM_STATES][NUM_EVENTS];

        /**
         * Permite transiccionar de estado-
         *
         *  Ejemplo:
         *      1. Mira el estado actual -> EXECUTE
         *      2. Mira el evento recibido -> EVT_EXEC_DONE
         *      3. Busca en la tabla -> fsm[EXECUTE][EVT_EXEC_DONE]
         *
         *      Esto te da:
         *          - el siguiente estado
         *          - la acción a ejecutar
         *
         * @param e nuevo estado para la maquina virtual
         */
        void on_event(vm_event e);

        /**
         * Referencia a la maquina virtual
         */
        VM &vm_reference;

        Scheduler(uint32_t id_scheduler, VM &vm_reference);

        /**
         * Lista de procesos listos para la ejecuccion:
         * ¿Por qué deque?
         *      - push_back O(1)
         *      - pop_front O(1)
         *      - ideal para round‑robin
         *      - no mueve memoria como un vector
         *
         * Cuando un proceso esta list, se agrega a esta lista:
         *  - Cuando se crea (spawn) -> ready_queue.push_back(pid);
         *  - Cuando termina WAIT_IO -> ready_queue.push_back(pid);
         *  - Cuando termina WAIT_MSG (implementas mailbox) -> ready_queue.push_back(pid);
         *  - Cuando termina WAIT_TIMER -> ready_queue.push_back(pid);
         *  - Cuando se agotan las reducciones -> ready_queue.push_back(pid);
         *
         */
        std::deque<GlobalPID> ready_queue;


        /**
         * @brief Ejecuta un único paso de la máquina de estados (FSM) para un proceso.
         *
         * Este metodo implementa la ejecución *atómica* de un estado dentro del ciclo
         * de instrucción de un proceso virtual. A diferencia de un bucle monolítico,
         * este metodo ejecuta **solo una transición** de la FSM y devuelve el control
         * al planificador (scheduler), permitiendo el cambio de proceso.
         *
         * @details
         * El metodo realiza las siguientes operaciones:
         *
         *  1. Selecciona el bloque de código asociado al estado actual del proceso
         *     mediante una tabla de despacho basada en "computed goto". Esto elimina
         *     la necesidad de `switch` o `if`, reduciendo la penalización por
         *     predicción de ramas y acelerando el intérprete.
         *
         *  2. Ejecuta la acción correspondiente al estado:
         *        - READY / RUNNING -> transición programada por el scheduler.
         *        - DECODE -> decodificación de la instrucción actual.
         *        - EXECUTE -> ejecución de la instrucción decodificada.
         *        - WAIT_IO -> el proceso no avanza hasta recibir EVT_IO_READY.
         *        - NEW -> proceso recién creado, no ejecutable aún.
         *
         *  3. Determina el evento resultante de la acción (por ejemplo:
         *     `EVT_DECODE_DONE`, `EVT_EXEC_DONE`, `EVT_IO_WAIT`, etc.).
         *
         *  4. Llama a `on_event(evento, proceso)`, que consulta la tabla de
         *     transiciones `fsm[][]` y actualiza el estado del proceso según la
         *     lógica declarativa definida en la FSM.
         *
         *  5. Finaliza inmediatamente tras procesar la transición, devolviendo el
         *     control al scheduler para permitir:
         *        - aplicar reducción de instrucciones,
         *        - cambiar de proceso,
         *        - reinsertar el proceso en la cola READY,
         *        - o suspenderlo si está bloqueado.
         *
         * Importante:
         *  - Este metodo **no contiene bucles**. Ejecuta exactamente un paso de la FSM.
         *  - No selecciona procesos ni gestiona reducciones; eso es responsabilidad
         *    exclusiva del scheduler.
         *  - Si el proceso entra en un estado terminal (HALT o DEAD), el metodo
         *    simplemente retorna y el scheduler decidirá cómo gestionarlo.
         *
         * @param process Proceso sobre el que se ejecutará un paso de la FSM.
         */
        void run_fsm_step(ProcessVM *process);


        /**
         * @brief Bucle principal del planificador (scheduler) de procesos.
         *
         * Este metodo implementa el ciclo de planificación cooperativa/preemptiva
         * de la máquina virtual. Se ejecuta típicamente en un hilo del thread pool
         * de la VM y es responsable de seleccionar procesos, aplicar reducciones
         * y ejecutar pasos individuales de la máquina de estados.
         *
         * @details
         * El bucle realiza repetidamente las siguientes operaciones:
         *
         *  1. Selecciona el siguiente proceso listo para ejecutarse mediante
         *     `schedule_next()`. Si no hay procesos listos, el scheduler cede
         *     temporalmente la CPU (`yield`) y continúa.
         *
         *  2. Inicializa el contador de reducciones del proceso. Cada reducción
         *     representa la ejecución de un paso de la FSM (una instrucción o
         *     transición significativa).
         *
         *  3. Mientras el proceso tenga reducciones disponibles:
         *        - Llama a `run_fsm_step(proceso)` para ejecutar un único paso.
         *        - Decrementa el contador de reducciones.
         *        - Si el proceso entra en un estado bloqueante (WAIT_IO, BLOCKED)
         *          o terminal (HALT, DEAD), se detiene la ejecución del proceso.
         *
         *  4. Si el proceso sigue en un estado ejecutable (READY o RUNNING),
         *     se reintroduce en la cola READY para ser ejecutado nuevamente
         *     en futuras iteraciones del scheduler.
         *
         *  5. El bucle continúa hasta que `should_kill` sea verdadero, lo que
         *     indica que la VM debe detener su ejecución.
         *
         * Importante:
         *  - Este metodo **no ejecuta instrucciones directamente**; delega toda la
         *    lógica de ejecución a `run_fsm_step()`.
         *  - Implementa un modelo de planificación similar al de Erlang/BEAM,
         *    permitiendo miles de procesos ligeros con cambios de contexto rápidos.
         *  - Garantiza que ningún proceso monopolice la CPU virtual gracias al
         *    sistema de reducciones.
         *
         * @note Este metodo debe ejecutarse en un hilo dedicado del thread pool
         *       de la VM. No debe ser llamado desde el hilo principal.
         */
        void run_loop();

        std::string to_string() const;

        void reset();


        /**
         * Inicializa la tabla de transicicones.
         */
        void init_fsm();

        /**
         * Permite saber si aun hay procesos vivos en ejecuccion.
         * @return true si quedan procesos vivos u en otro estados
         * distinto a DEAD. Este metodo tiene un coste O(N)
         */
        bool has_alive_processes() const;

        /**
         * Permite ejecutar el siguiente proceso de la VM
         * @return devuelve un puntero al proceso que esta en ejecuccion
         */
        ProcessVM *schedule_next();

        /**
         * Permite eliminar un proceso del gestor de procesos.
         */
        void kill(GlobalPID pid);

        /**
         * Permite crear un proceso nuevo dentro del gestor de procesos.
         * @return pid del nuevo proceso creado.
         */
        GlobalPID spawn();

        /**
         * Permite marcar un proceso como listo
         * @param pid PID del proceso que esta listo.
         */
        void make_ready(GlobalPID pid);

        /**
         * Permite indicar internamete que la VM debe morir o deberia estar
         * muerta, esto lo hacemos ya que no se puede llamar a on_event desde
         * dentro de la propio VM ni se puede llamar a kill() el cual llama a on_event.
         */
        bool should_kill = false;

        /**
         * Permite poner un evento en la cola
         * @param e evento a realizar, se pondra a la cola y se realizara cuando sea
         * necesario.
         */
        //void emit_event(vm_event e);

        /**
         * Permite agregar una nueva hook a la VM, esta funcion no es thread-safe,
         * asi que no se debe usar para añadir un hook mientras la VM se ejecuta, a
         * no ser que se haga uso de algun mecanismo de sincronizacion. Se recomienda usar
         * la version safe-thread la cual es "add_debug_hook" y añade un mutex que permite
         * añadir hooks en run time mientras la VM se ejecuta.
         * @param hook funcion hook que añadir a la cola de hooks.
         */
        void free_add_debug_hook(DebugHook hook);

        void add_debug_hook(DebugHook hook);

        /**
         * Permite activar y desactivar hooks, al añadir un hook
         * usando add_debug_hook automaticamente esta flag se activa,
         * se puede desactivar en cualquier momento para que los hooks
         * no interfieran en la ejecuccion de la VM.
         */
        bool has_hooks = false;

        ~Scheduler();

        /**
         * Cola de eventos pendientes, si alguna instruccion o algo externo
         * quiero generar algun evento, se debe poner a la cola y hasta que la VM
         * no termine su evento actual no se podra realizar los eventos de la cola.
         */
        //std::queue<vm_event> pending_events;

        Timer debug_timer{}; // mide la fase actual en el modo de depuracion

        /**
         * toiempo tardado en el decoder
         */
        uint64_t time_decode = 0;

        /**
         * tiempo de transicion de eventos.
         */
        uint64_t time_event = 0;

        /**
         * tiempo tardado en la ejecuccion
         */
        uint64_t time_exec = 0;

        // --- PROFILER POR HILO / VM---
        uint64_t profiler_sample        = 0; // contador de instrucciones
        uint64_t profiler_instr_counter = 0; // incrementa cada 256 instrucciones
        // --- PROFILER POR HILO / VM ---

        /**
         * permite indicar que se puede seguir ejecutado las funcionalidades
         * de "profiler" internas o externas a la VM.
         */
        std::atomic<bool> profiler_running = true;

        /**
         * Permite saber si esta a la espera.
         */
        std::atomic<bool> is_waiting { false };

        /**
         * Permite reactivar un gestor de procesos que se haya quedado esperando.
         * API idéntica a std::counting_semaphore: sem.acquire() bloquea,
         * sem.release() despierta desde cualquier hilo (make_ready/stop).
         */
        BinarySemaphore sem;

    private:
    };

    static const char *debug_stage_name(DebugStage s) {
        switch (s) {
            case DebugStage::DecodeBegin: return "DECODE_BEGIN";
            case DebugStage::DecodeEnd: return "DECODE_END";
            case DebugStage::ExecuteBegin: return "EXEC_BEGIN";
            case DebugStage::ExecuteEnd: return "EXEC_END";
            case DebugStage::OnEventBegin: return "EVENT_BEGIN";
            case DebugStage::OnEventEnd: return "EVENT_END";
            default: return "UNKNOWN";
        }
    }



    /**
     * @brief Ejecuta todos los hooks de depuración registrados para la máquina virtual.
     *
     * Esta función se invoca en distintos puntos del ciclo de ejecución de la VM
     * (fetch, decode, execute, eventos del FSM, etc.) y permite que múltiples
     * callbacks externos reaccionen a cada fase sin interferir con la lógica
     * interna del intérprete.
     *
     * Cada hook registrado en `vm->debug_hooks` recibe el puntero a la VM y el
     * estado de depuración actual, permitiendo implementar:
     *  - trazas de ejecución,
     *  - breakpoints,
     *  - stepping,
     *  - inspección del estado interno,
     *  - profiling por instrucción o por evento,
     *  - integración con debuggers externos.
     *
     * @param vm    Puntero a la máquina virtual que está ejecutando el ciclo.
     * @param stage Fase del pipeline o del FSM en la que se dispara el hook.
     *
     * @note Esta función no realiza comprobaciones adicionales: si un hook lanza
     *       una excepción o produce efectos secundarios inesperados, puede afectar
     *       al flujo de depuración. Se recomienda que los hooks sean seguros y
     *       no bloqueantes.
     */
    //#define PROFILE_FAST
#ifdef PROFILE_FAST
#   define vm_hook(...) do {} while(0)
#else
    void vm_hook(ProcessVM *process, DebugStage stage);
#endif

    /**
     * @brief Hilo de perfilado para una instancia de VM.
     *
     * Este hilo se ejecuta en paralelo a la VM y se encarga de:
     *   - Calcular las instrucciones por segundo (IPS) usando muestreo.
     *   - Calcular el porcentaje de CPU consumido por la VM.
     *   - Imprimir los resultados usando vesta::scout() (thread-safe).
     *
     * El cálculo funciona así:
     *   - Cada 256 instrucciones ejecutadas, la VM incrementa profiler_instr_counter.
     *   - Cada segundo, este hilo lee ese contador y calcula:
     *         IPS = delta * 256
     *   - El tiempo ocupado (busy time) se acumula en vm->time_exec (ns).
     *         CPU% = (time_exec / 1e9) * 100
     *
     *  IPS alto + CPU bajo -> la VM está idle o ejecuta pocas instrucciones por segundo.
     *  IPS alto + CPU alto -> la VM está ejecutando un bucle caliente.
     *  IPS bajo + CPU alto -> la VM está haciendo trabajo costoso por instrucción.
     *
     * Podemos ejecutar un profiler como se ve a continuacion:
     *
     * @code{.cpp}
     * std::thread(&runtime::profiler_thread, vm).detach();
     * @endcode
     *
     * profiler_thread depende de la flag interna vm->should_kill que indica
     * si la VM va a morir o deberia morir y de vm->profiler_running que
     * indica si la VM desea que se haga profiler no, en el caso de
     * esta funcion se debe cumplir la siguiente condicion:
     * @code{.cpp}
     *      !vm->should_kill && vm->profiler_running
     * @endcode
     * En caso de que alguna cambiem el profiler se detendra.
     *
     * @param vm Puntero a la instancia de VM que se está perfilando.
     */
    void profiler_thread(Scheduler *scheduler);
}

#endif //SCHEDULER_H
