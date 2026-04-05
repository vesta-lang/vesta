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

#include "cli/sync_io.h"
#include "runtime/manager_runtime.h"


namespace runtime {
    VM::VM(
        ManageVM &mgr_vm,
        uint64_t id_vm
    ) : vm_mem(tlb, mgr_vm.manager_mem),
        manager_mem_public(mgr_vm.manager_mem),
        loader_priv(mgr_vm),
        loader_public(mgr_vm.loader),
        id({id_vm}),
        mgr_vm(mgr_vm) { {
            this->manager_mem_priv = {};

            // aun no se cargo nada asi que se inicializara a 0 todo_
            stack_pointer = vm::MappedPtr{

            };

            base_pointer = vm::MappedPtr{

            };

            rip = vm::MappedPtr{

            };

            time_sleep = 0;
            pending_call = nullptr;
        }

        init_fsm();
    }

    void VM::init_fsm() {
        // Limpia todo_
        for (uint64_t s = 0; s < NUM_STATES; s++) {
            for (uint64_t e = 0; e < NUM_EVENTS; e++) {
                fsm[s][e].next = (vm_state) s;

                // por defecto: no cambia de estado
                fsm[s][e].action = nullptr;
            }
        }
        // FETCH -> RUNNING
        fsm[READY][EVT_SCHEDULED] = {RUNNING, nullptr};

        // RUNNING -> FETCH
        fsm[RUNNING][EVT_SCHEDULED] = {FETCH, nullptr};

        // FETCH -> DECODE
        fsm[FETCH][EVT_FETCH_DONE] = {
            DECODE,
            [](VM *vm) {
                vm->fetch_instruction();
            }
        };

        // DECODE -> EXECUTE
        fsm[DECODE][EVT_DECODE_DONE] = {
            EXECUTE,
            [](VM *vm) {
                vm->decode_instruction();
            }
        };

        // EXECUTE -> FETCH
        fsm[EXECUTE][EVT_EXEC_DONE] = {
            FETCH,
            [](VM *vm) {
                /*vm->execute_instr();*/
            }
        };

        // EXECUTE -> WAIT_IO
        fsm[EXECUTE][EVT_IO_WAIT] = {
            WAIT_IO,
            [](VM *vm) {
                /*vm->suspend_thread();*/
            }
        };

        // WAIT_IO -> READY
        fsm[WAIT_IO][EVT_IO_READY] = {
            READY,
            [](VM *vm) {
                /*vm->resume_thread();*/
            }
        };

        // EXECUTE -> HALT
        fsm[EXECUTE][EVT_HALT] = {
            HALT,
            [](VM *vm) {
                /*vm->stop_vm();*/
            }
        };

        // Cualquier estado -> DEAD por error
        for (int s = 0; s < NUM_STATES; s++) {
            fsm[s][EVT_ERROR] = {
                DEAD,
                [](VM *vm) {
                    /*vm->fatal_error();*/
                }
            };
        }
    }

    void VM::on_event(vm_event e) {
        std::lock_guard guard(state_lock);

        vm_state old = state;

        // Obtener la transición desde la tabla
        const Transition &t = fsm[state][e];

        vesta::scout() << "[VM] on_event: " << vm_state_to_str(old) <<
                " --" << event_name(e) << "--> " << vm_state_to_str(t.next) << std::endl;

        // Ejecutar acción asociada (si existe)
        if (t.action)
            t.action(this);

        // Cambiar al siguiente estado
        state = t.next;
    }

    std::string VM::to_string() {
        return this->vm_summary();
    }

    void VM::print() {
        // Reutiliza to_string() para evitar duplicar lógica
        vesta::scout() << to_string();
    }

    void VM::kill() {
        std::lock_guard guard(state_lock);
        on_event(EVT_ERROR);
    }

    void VM::start() {
        pthread_create(&thread_for_vm, nullptr, [](void *arg) -> void * {
            VM *vm = static_cast<VM *>(arg);
            vm->run_loop();
            return nullptr;
        }, this);

        // no usar por que entonces join ya no se puede usar.
        //pthread_detach(thread_for_vm); // auto destruir hilo al finalizar.
    }

    void VM::join() {
        pthread_join(thread_for_vm, nullptr);
    }
} // RUNTIME
