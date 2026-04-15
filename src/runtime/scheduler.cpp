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
        return; //goto *dispatch_table[instance->state];

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
        std::lock_guard guard(state_lock);
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
        for (auto &p: processes) {
            if (p->state != DEAD)
                return true;
        }
        return false;
    }

    void Scheduler::run_loop() {
        while (!should_kill) {
            ProcessVM *p = schedule_next();
            if (!p) {
                // si se obtuvo un puntero nulo, puede decir varias cosas, una de ellas es que los procesos
                // que hay no estan muertos pero estan en otro estado difente a listo y aun no estan preparados para
                // ser procesados. El otro caso que se puede dar es que todos los procesos hayan muerto,

                // No hay procesos vivos, en ese caso
                // detenemos este gestor de procesos e hilo, esto no afectara a los demas gestores de procesos.
                //if (!has_alive_processes()) {
                //    // No queda nada que ejecutar -> detener scheduler
                //    break;
                //}

                // Hay procesos vivos pero bloqueados -> idle,
                // Esto evita que la VM haga busy-waiting (100% CPU sin hacer nada).
                std::this_thread::yield();
                continue;
            }

            // Ejecutar hasta agotar reducciones
            while (p->reductions_remaining > 0) {
                run_fsm_step(p); // ejecuta UNA instrucción

                p->reductions_remaining--;

                if (p->state == WAIT_IO || p->state == BLOCKED || p->state == DEAD)
                    break;
            }

            // si se acaba las reducciones se re-encola el proceso.
            if (p->reductions_remaining == 0) {
                on_event(EVT_YIELD);
            }

            // Si sigue vivo, vuelve a la cola
            if (p->state == READY || p->state == RUNNING)
                ready_queue.push_back(p->pid);
        }
    }


    ProcessVM *Scheduler::schedule_next() {
        if (ready_queue.empty())
            return nullptr;

        GlobalPID pid = ready_queue.front();
        ready_queue.pop_front();

        ProcessVM *p            = pid_index[pid];
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
        p->state     = READY;
        ready_queue.push_back(pid);
    }

    void Scheduler::kill(GlobalPID pid) {
        //on_event(EVT_ERROR);
        auto it = pid_index.find(pid);
        if (it == pid_index.end()) return;

        ProcessVM *p = it->second;

        // eliminar del índice
        pid_index.erase(it);

        // eliminar del vector
        for (size_t i = 0; i < processes.size(); i++) {
            if (processes[i].get() == p) {
                processes.erase(processes.begin() + i);
                break;
            }
        }
    }

    void Scheduler::free_add_debug_hook(DebugHook hook) {
        debug_hooks.push_back(hook);
        has_hooks = true;
    }

    void Scheduler::add_debug_hook(DebugHook hook) {
        std::lock_guard guard(state_lock);
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
            std::lock_guard guard(state_lock);

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


    void profiler_thread(ProcessVM *vm) {
        uint64_t last = 0;

        // mientras la vm no deba haber acabado
        while (vm->scheduler.profiler_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // --- IPS ---
            uint64_t now   = vm->scheduler.profiler_instr_counter;
            uint64_t delta = now - last;
            last           = now;

            uint64_t ips = delta * 256;

            // --- CPU ---
            double cpu              = (vm->scheduler.time_exec / 1e9) * 100.0;
            vm->scheduler.time_exec = 0;

            // --- Salida thread-safe ---
            vesta::scout()
                    << "\r[scheduler[" << vm->pid.local_pid << "]: " << vesta::hex64(vm->scheduler.id_scheduler) << "] "
                    << "IPS: " << ips
                    << " | CPU: " << cpu << "%\n";
        }
    }
}
