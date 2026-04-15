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
        ManageVM &mgr_vm_,
        uint64_t  id_vm
    ) : manager_mem_public(mgr_vm_.manager_mem),
        loader_priv(std::make_unique<loader::Loader>(mgr_vm_)),
        loader_public(mgr_vm_.loader),
        mgr_vm(mgr_vm_) {
        id = id_vm;
    }



    std::string VM::to_string() const {
        //return this->vm_summary();
        return "";
    }

    void VM::print() {
        // Reutiliza to_string() para evitar duplicar lógica
        vesta::scout() << to_string();
    }



    /*void VM::emit_event(vm_event e) {
        std::lock_guard guard(state_lock);
        pending_events.push(e);
    }*/


    void VM::start(size_t num_schedulers) {
        schedulers.reserve(num_schedulers);

        for (uint32_t i = 0; i < num_schedulers; i++) {
            auto sched = std::make_unique<Scheduler>(i, *this);
            schedulers.push_back(std::move(sched));

            pool.enqueue([this, i] {
                schedulers[i]->run_loop();
            });
        }
    }


    //void VM::join() {
    //    pthread_join(thread_for_vm, nullptr);
    //}

    ProcessVM *VM::get_process(GlobalPID pid) {
        return schedulers[pid.scheduler_id]->pid_index[pid];
    }

    void VM::make_ready(GlobalPID pid) {
        schedulers[pid.scheduler_id]->make_ready(pid);
    }

    GlobalPID VM::spawn_process() {
        size_t index = next_sched;
        next_sched   = (next_sched + 1) % schedulers.size();

        return schedulers[index]->spawn();
    }
} // RUNTIME
