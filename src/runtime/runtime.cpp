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
        uint64_t  id_vm,
        size_t    num_schedulers
    ) : manager_mem_public(mgr_vm_.manager_mem),
        loader_priv(std::make_unique<loader::Loader>(mgr_vm_)),
        loader_public(mgr_vm_.loader),
        mgr_vm(mgr_vm_), num_schedulers(num_schedulers) {
        id = id_vm;

        schedulers.reserve(num_schedulers);

        for (uint32_t i = 0; i < num_schedulers; i++) {
            auto sched = std::make_unique<Scheduler>(i, *this);
            schedulers.push_back(std::move(sched));
        }
    }

    std::string VM::to_string() const {
        return this->vm_summary();
        return "";
    }

    void VM::print() {
        // Reutiliza to_string() para evitar duplicar lógica
        vesta::scout() << to_string();
    }

    void VM::start() {
        for (uint32_t i = 0; i < num_schedulers; i++) {
            // submit() devuelve un future
            std::future<void> fut = pool.submit([this, i] {
                schedulers[i]->run_loop();
            });

            scheduler_futures.push_back(std::move(fut));
        }
    }

    void VM::stop() {
        // Indicar que la VM debe morir
        vm_running = false;

        // Marcar todos los schedulers para matar
        for (auto &sched : schedulers) {
            sched->should_kill = true;
        }

        // Despertar a todos los schedulers que estén en wait()
        for (auto &sched : schedulers) {
            sched->cv.notify_all();
        }

        // Esperar a que todos los hilos terminen
        for (auto &f : scheduler_futures) {
            f.get();
        }

        scheduler_futures.clear();
    }

    ProcessVM *VM::get_process(GlobalPID pid) {
        return schedulers[pid.scheduler_id]->pid_index[pid];
    }

    void VM::wait() {
        for (auto &f: scheduler_futures)
            f.get(); // bloquea hasta que cada scheduler termine
    }

    bool VM::has_alive_processes() const {
        for (auto &sched: schedulers) {
            if (sched->has_alive_processes())
                return true;
        }
        return false;
    }

    void VM::make_ready(GlobalPID pid) {
        schedulers[pid.scheduler_id]->make_ready(pid);
        schedulers[pid.scheduler_id]->cv.notify_one();
    }

    GlobalPID VM::spawn_process() {
        if (schedulers.empty())
            throw std::runtime_error("VM::spawn_process: no hay gestores de procesos, posiblemente no llamo a start()");

        size_t index = next_sched;
        next_sched   = (next_sched + 1) % schedulers.size();

        return schedulers[index]->spawn();
    }
} // RUNTIME
