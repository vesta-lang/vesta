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
        loader_priv(std::make_unique<loader::Loader>(mgr_vm)),
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
        // READY -> RUNNING
        fsm[READY][EVT_SCHEDULED] = {RUNNING, nullptr};

        // RUNNING -> DECODE
        fsm[RUNNING][EVT_SCHEDULED] = {DECODE, nullptr};

        // DECODE -> EXECUTE
        fsm[DECODE][EVT_DECODE_DONE] = Transition{
            EXECUTE,
            [](VM *vm) {
                vm->decode_instruction();
            }
        };

        // EXECUTE -> DECODE
        fsm[EXECUTE][EVT_EXEC_DONE] = Transition{
            DECODE,
            [](VM *vm) {
                /*vm->execute_instr();*/
            }
        };

        // EXECUTE -> WAIT_IO
        fsm[EXECUTE][EVT_IO_WAIT] = Transition{
            WAIT_IO,
            [](VM *vm) {
                /*vm->suspend_thread();*/
            }
        };

        // WAIT_IO -> READY
        fsm[WAIT_IO][EVT_IO_READY] = Transition{
            READY,
            [](VM *vm) {
                /*vm->resume_thread();*/
            }
        };

        // EXECUTE -> HALT
        fsm[EXECUTE][EVT_HALT] = Transition{
            HALT,
            [](VM *vm) {
                /*vm->stop_vm();*/
            }
        };

        // Cualquier estado -> DEAD por error
        for (int s = 0; s < NUM_STATES; s++) {
            fsm[s][EVT_ERROR] = Transition{
                DEAD,
                [](VM *vm) {
                    /*vm->fatal_error();*/
                }
            };
        }
    }

    void VM::on_event(vm_event e) {
        //vm_hook(this, DebugStage::OnEventBegin);
        {
            std::lock_guard guard(state_lock);

            vm_state old = state;

            // Obtener la transición desde la tabla
            const Transition &t = fsm[state][e];

            // Ejecutar acción asociada (si existe)
            if (t.action)
                t.action(this);

            // Cambiar al siguiente estado
            state = t.next;
        }
        //vm_hook(this, DebugStage::OnEventEnd);
    }

    std::string VM::to_string() const {
        return this->vm_summary();
    }

    void VM::print() {
        // Reutiliza to_string() para evitar duplicar lógica
        vesta::scout() << to_string();
    }

    void VM::kill() {
        on_event(EVT_ERROR);
    }

    /*void VM::emit_event(vm_event e) {
        std::lock_guard guard(state_lock);
        pending_events.push(e);
    }*/


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

    void VM::free_add_debug_hook(DebugHook hook) {
        debug_hooks.push_back(hook);
        has_hooks = true;
    }

    void VM::add_debug_hook(DebugHook hook) {
        std::lock_guard guard(state_lock);
        free_add_debug_hook(hook);
    }

    void VM::load_raw_code(uint64_t address, const std::vector<uint8_t> &code) {
        // copiar a memoria virtual
        vm_mem.vm_to_host_memcpy(address, code.data(), code.size());

        // resetear PC
        rip.ptr_vm.raw = address;

        // resetear icache
        for (auto &entry: icache) {
            entry.pc = UINT64_MAX; // invalida
            decoded_ptr = nullptr; // invalidar punteros decoder anteriores.
        }

        // resetear estado de ejecución
        decoded_ptr = nullptr;
        should_kill = false;
        state = RUNNING;
    }
} // RUNTIME
