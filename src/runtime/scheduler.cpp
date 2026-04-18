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

#include "runtime/scheduler.h"

#include "runtime/decode_instruction.h"

namespace runtime {
    Scheduler::Scheduler(
        uint32_t id_scheduler, VM &vm_reference): id_scheduler(id_scheduler), vm_reference(vm_reference) {
        init_fsm();
    }

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

    // stepper
    void Scheduler::run_fsm_step(ProcessVM *process) {
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
         * para que sea compilable.
         *
         * Informacion sobre predicion de ramas (branch-prediction):
         *      https://stackoverflow.com/questions/11668090/how-to-deal-with-branch-prediction-when-using-a-switch-case-in-cpu-emulation
         */
        static void *dispatch_table[NUM_STATES] = {
            &&READY_LABEL,
            &&RUNNING_LABEL,
            &&BLOCKED_LABEL,
            &&DEAD_LABEL,
            &&DECODE_LABEL,
            &&EXECUTE_LABEL,
            &&WAIT_IO_LABEL,
            &&HALT_LABEL,
            &&NEW_LABEL
        };

        /**
         */
        goto *dispatch_table[instance->state];

    READY_LABEL:
        // READY no ejecuta instrucciones, solo pasa a running
        on_event(EVT_SCHEDULED); // transición READY -> RUNNING
        goto *dispatch_table[instance->state];

    RUNNING_LABEL:
        // RUNNING simplemente pasa a FETCH
        on_event(EVT_SCHEDULED); // o EVT_SCHEDULED ?
        goto *dispatch_table[instance->state];

    BLOCKED_LABEL:
        // No hay nada que hacer hasta que un evento externo desbloquee
        return;

    DECODE_LABEL:
        on_event(EVT_DECODE_DONE);
        // Cae directo a EXECUTE: una sola llamada cubre decode+execute completo.
        // 1 llamada = 1 instrucción = 1 reducción (semántica correcta).
        goto *dispatch_table[instance->state];

    EXECUTE_LABEL:
        // si la instruccion ejecuta no es bloqueante, se avanzara en el
        // estado de la VM, pero en caso de que execute_instruction devuelva
        // false, se lanzara un evento de tipo EVT_IO_WAIT como evento bloqueante.
        on_event(execute_instruction(process));
        return; //goto *dispatch_table[instance->state];

    WAIT_IO_LABEL:
        /*if (io_ready())
            on_event(EVT_IO_READY);*/
        return; //goto *dispatch_table[instance->state];

    HALT_LABEL:
    DEAD_LABEL:
        return;

    NEW_LABEL:
        // No ejecutar nada todavía
        return;
    }

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    /*void VM::emit_event(vm_event e) {
        pending_events.push(e);
    }*/

    void Scheduler::init_fsm() {
        // Limpia todo_
        for (uint64_t s = 0; s < NUM_STATES; s++) {
            for (uint64_t e = 0; e < NUM_EVENTS; e++) {
                fsm[s][e].next = (vm_state) s;

                // por defecto: no cambia de estado
                fsm[s][e].action = nullptr;
            }
        }


        // READY -> RUNNING
        fsm[READY][EVT_SCHEDULED] = {RUNNING, nullptr};

        // RUNNING -> DECODE
        fsm[RUNNING][EVT_SCHEDULED] = {DECODE, nullptr};

        // DECODE -> EXECUTE
        fsm[DECODE][EVT_DECODE_DONE] = Transition{
            EXECUTE,
            [](ProcessVM *vm) {
                decode_instruction(vm);
            }
        };

        // EXECUTE -> DECODE
        fsm[EXECUTE][EVT_EXEC_DONE] = Transition{
            DECODE,
            [](ProcessVM *vm) {
                /*vm->execute_instr();*/
            }
        };

        // EXECUTE -> WAIT_IO
        fsm[EXECUTE][EVT_IO_WAIT] = Transition{
            WAIT_IO,
            [](ProcessVM *vm) {
                /*vm->suspend_thread();*/
            }
        };

        // WAIT_IO -> READY
        fsm[WAIT_IO][EVT_IO_READY] = Transition{
            READY,
            [](ProcessVM *vm) {
                /*vm->resume_thread();*/
            }
        };

        // EXECUTE -> HALT
        fsm[EXECUTE][EVT_HALT] = Transition{
            HALT,
            [](ProcessVM *vm) {
                /*vm->stop_vm();*/
            }
        };

        // evento FSM para “yield” (fin de reducciones)
        fsm[EXECUTE][EVT_YIELD] = {READY, nullptr};
        fsm[DECODE][EVT_YIELD]  = {READY, nullptr};
        fsm[RUNNING][EVT_YIELD] = {READY, nullptr};

        /**
         * Esto significa:
         * cuando el scheduler decida que el proceso está listo
         * lo pasa de NEW -> READY
         */
        fsm[NEW][EVT_SCHEDULED] = {READY, nullptr};

        // Cualquier estado -> DEAD por error
        for (int s = 0; s < NUM_STATES; s++) {
            fsm[s][EVT_ERROR] = Transition{
                DEAD,
                [](ProcessVM *vm) {
                    /*vm->fatal_error();*/
                }
            };
        }
    }

    bool Scheduler::has_alive_processes() const {
        return alive_count > 0;
    }

    Scheduler::~Scheduler() {
        should_kill = true;
        for (auto &p: processes) {
            // si decoded_ptr es nullptr posiblemente se haya liberado memoria previamente.
            if (p->decoded_ptr != nullptr) {
                p->decoded_ptr->flags_info.blocking = true;
                p->state                            = DEAD;
                //kill(p->pid);
            }
        }
    }

    void Scheduler::run_loop() {
        // should_kill puede matar solo un gestor de procesos,
        // pèro vm_running puede matar a todos los gestores de la instancia
        while (!should_kill && vm_reference.vm_running) {
            instance = schedule_next();
            if (!instance) {
                // si se obtuvo un puntero nulo, puede decir varias cosas, una de ellas es que los procesos
                // que hay no estan muertos pero estan en otro estado difente a listo y aun no estan preparados para
                // ser procesados. El otro caso que se puede dar es que todos los procesos hayan muerto,

                // No hay procesos vivos, en ese caso
                // detenemos este gestor de procesos e hilo, esto no afectara a los demas gestores de procesos.
                // ¿Quedan procesos vivos en toda la VM?
                if (!vm_reference.has_alive_processes()) {
                    vm_reference.vm_running = false;
                    break; // este scheduler termina
                }
                is_waiting = true;
                sem.acquire(); // duerme hasta que haya trabajo (sem.release desde make_ready/stop)
                is_waiting = false;

                // Si nos despertaron porque la VM muere -> salir
                if (should_kill || !vm_reference.vm_running)
                    break;

                // Si nos despertaron porque hay trabajo -> obtener proceso
                instance = schedule_next();

                continue;
            }

            // Ejecutar hasta agotar reducciones
            while (instance->reductions_remaining > 0) {
                run_fsm_step(instance); // ejecuta decode+execute (1 instrucción completa)

                instance->reductions_remaining--;

                if (instance->state == DEAD || instance->state == HALT) {
                    alive_count--;
                    instance = nullptr;
                    break;
                }
                if (instance->state == WAIT_IO || instance->state == BLOCKED) {
                    instance = nullptr;
                    break;
                }
            }
            if (instance == nullptr) {
                continue;
            }

            // si se acaba las reducciones se re-encola el proceso.
            if (instance->reductions_remaining == 0) {
                on_event(EVT_YIELD);
            }

            // Si sigue vivo, vuelve a la cola
            if (instance->state == READY) {
                std::lock_guard<std::mutex> lock(queue_mutex);
                ready_queue.push_back(instance);
            }
        }
    }

    std::string Scheduler::to_string() const {
        std::ostringstream ss;

        ss << "Scheduler[" << id_scheduler << "] {\n"
                << "  waiting: " << (is_waiting ? "yes" : "no") << "\n"
                << "  should_kill: " << (should_kill ? "yes" : "no") << "\n"
                << "  profiler_running: " << (profiler_running ? "yes" : "no") << "\n"
                << "  processes_total: " << processes.size() << "\n"
                << "  ready_queue: " << ready_queue.size() << "\n"
                << "  alive_processes: " << vm_reference.has_alive_processes() << "\n"
                << "  reductions_now: " << reductions_now << "\n"
                << "  profiler_instr_counter: " << profiler_instr_counter << "\n"
                << "  time_exec(ns): " << time_exec << "\n"
                << "  time_decode(ns): " << time_decode << "\n"
                << "  time_event(ns): " << time_event << "\n"
                << "}";

        return ss.str();
    }

    void Scheduler::reset() {
        // Reset de colas y estructuras
        ready_queue.clear();
        pid_index.clear();
        processes.clear();

        // Reset de contadores
        next_pid               = 0;
        reductions_now         = 0;
        profiler_instr_counter = 0;
        time_exec              = 0;
        time_decode            = 0;
        time_event             = 0;

        // Reset de flags
        is_waiting       = false;
        should_kill      = false;
        profiler_running = false;
        alive_count      = 0;

        // Reset de la instancia FSM
        instance = nullptr;
    }


    ProcessVM *Scheduler::schedule_next() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (ready_queue.empty())
            return nullptr;

        ProcessVM *p = ready_queue.front();
        ready_queue.pop_front();

        p->reductions_remaining = reductions_remaining_default;
        instance = p;
        return p;
    }

    GlobalPID Scheduler::spawn() {
        GlobalPID pid = {id_scheduler, next_pid++};

        auto       proc = std::make_unique<ProcessVM>(*this, pid);
        ProcessVM *raw  = proc.get();

        processes.push_back(std::move(proc));
        pid_index[pid] = raw;

        return pid;
    }

    void Scheduler::make_ready(GlobalPID pid) {
        ProcessVM *p = pid_index[pid];

        // Solo la primera vez que un proceso pasa de NEW a activo se cuenta.
        // Llamadas posteriores (ej. tras WAIT_IO) no incrementan el contador.
        if (p->state == NEW || p->state == HALT || p->state == DEAD)
            // reactivamos el hilo si quedo muerto y se esta intentado
            // reutilizar.
            alive_count++;

        p->state = READY;

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            ready_queue.push_back(p);
        }
    }

    void Scheduler::kill(GlobalPID pid) {
        auto it = pid_index.find(pid);
        if (it == pid_index.end()) return;

        ProcessVM *p = it->second;

        // Si el proceso era "vivo" (no NEW/DEAD/HALT), decrementar contador.
        if (p->state != NEW && p->state != DEAD && p->state != HALT)
            alive_count--;

        pid_index.erase(it);

        // swap-and-pop: O(1) en lugar de O(N) shift
        for (size_t i = 0; i < processes.size(); i++) {
            if (processes[i].get() == p) {
                if (i != processes.size() - 1)
                    processes[i] = std::move(processes.back());
                processes.pop_back();
                break;
            }
        }
    }



    void Scheduler::free_add_debug_hook(DebugHook hook) {
        debug_hooks.push_back(hook);
        has_hooks = true;
    }

    void Scheduler::add_debug_hook(DebugHook hook) {
        free_add_debug_hook(hook);
    }

#ifdef PROFILE_FAST
#else
    void vm_hook(ProcessVM *process, DebugStage stage) {
        if (!process->scheduler.has_hooks) return;

        for (auto &hook: process->scheduler.debug_hooks)
            hook(process, stage);
    }
#endif


    void Scheduler::on_event(vm_event e) {
        //vm_hook(this, DebugStage::OnEventBegin);
        {
            vm_state old = instance->state;

            // Obtener la transición desde la tabla
            const Transition &t = fsm[instance->state][e];

            // Ejecutar acción asociada (si existe)
            if (t.action)
                t.action(instance);

            // Cambiar al siguiente estado
            instance->state = t.next;
        }
        //vm_hook(this, DebugStage::OnEventEnd);
    }


    void profiler_thread(Scheduler *scheduler) {
        uint64_t last_instr = 0;
        uint64_t last_exec  = 0;

        while (scheduler->profiler_running &&
            !scheduler->should_kill &&
            scheduler->vm_reference.vm_running && scheduler->is_waiting != true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // IPS
            uint64_t now_instr   = scheduler->profiler_instr_counter;
            uint64_t delta_instr = now_instr - last_instr;
            last_instr           = now_instr;

            uint64_t ips = delta_instr;

            // CPU%
            uint64_t now_exec   = scheduler->time_exec;
            uint64_t delta_exec = now_exec - last_exec;
            last_exec           = now_exec;

            double cpu = ((double(delta_exec) / 1e9) * 100.0);
            if (cpu > 100.0) cpu = 100.0;

            vesta::scout()
                    << "[scheduler " << scheduler->id_scheduler << "] "
                    << "IPS=" << ips
                    << " | CPU=" << cpu << "%\n";
        }
    }
}
