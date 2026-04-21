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
        vm_running = true;
        for (uint32_t i = 0; i < num_schedulers; i++) {
            //vesta::scout() << "Creando future para scheduler " << i << "\n";

            // submit() devuelve un future
            std::future<void> fut = pool.submit([this, i] {
                schedulers[i]->run_loop();
            });
            //vesta::scout() << "Future creado\n";

            scheduler_futures.push_back(fut.share());
            //vesta::scout() << "Future movido al vector\n";
        }
    }

    void VM::stop() {
        // Indicar que la VM debe morir
        vm_running = false;

        // Marcar todos los schedulers para matar
        for (auto &sched: schedulers) {
            sched->should_kill = true;
        }

        // Despertar a todos los schedulers que estén en sem.acquire()
        for (auto &sched: schedulers) {
            sched->sem.release();
        }

        // si los hilos no terminaron debemos esperar, en caso contrario
        // no debemos hacer nada simplemente.
        //bool status = all_schedulers_dead();
        //if (!status) {
        // Esperar a que todos los hilos terminen
        for (auto &f: scheduler_futures) {
            f.get();
        }
        //}

        scheduler_futures.clear();
    }

    ProcessVM *VM::get_process(GlobalPID pid) {
        return schedulers[pid.scheduler_id]->pid_index[pid];
    }

    void VM::wait() {
        //vesta::scout() << "Entrando en wait()\n";
        for (auto &f: scheduler_futures) {
            //::scout() << "Haciendo get() del future " << "\n";
            f.wait(); // bloquea hasta que cada scheduler termine
        }
        //vesta::scout() << "Todos los futures consumidos\n";
    }

    bool VM::all_schedulers_dead() {
        bool is_dead = pool.idle();
        return is_dead;
    }

    bool VM::has_alive_processes() {
        // O(N_schedulers): cada scheduler mantiene alive_count atómico,
        // evitando iterar todos los procesos (era O(N_procesos total)).
        for (auto &sched: schedulers) {
            if (sched->alive_count > 0)
                return true;
        }
        return false;
    }

    void VM::make_ready(GlobalPID pid) {
        schedulers[pid.scheduler_id]->make_ready(pid);
        schedulers[pid.scheduler_id]->sem.release();

        // muy importante si se añade procesos sin un reset, el gestor de procesos una
        // vez finalice todos sus procesos, la instancia, activa esta bandera que indica
        // al propio gestor que a terminado y no debe volver a iniciar o continuar con
        // ningun otro proceso. Si se quiere añadir nuevos procesos en un estado como
        // este, es necesario reactivarla o nunca sera ejecutado el proceso.
        schedulers[pid.scheduler_id]->should_kill = false;
    }

    GlobalPID VM::spawn_process() {
        if (schedulers.empty())
            throw std::runtime_error("VM::spawn_process: no hay gestores de procesos, posiblemente no llamo a start()");

        size_t index = next_sched;
        next_sched   = (next_sched + 1) % schedulers.size();

        return schedulers[index]->spawn();
    }

    void VM::reset() {
        // Asegurar que todos los schedulers han terminado
        stop(); // vm_running = false, should_kill = true, cv.notify_all(), f.get(), clear()

        // Resetear el estado interno de cada scheduler
        for (auto &sched: schedulers) {
            sched->reset(); // limpia procesos, colas, pid_index, FSM, contadores, flags...
        }

        // Resetear estado interno de la VM
        next_sched = 0;
        vm_running = true; // lista para volver a arrancar

        // Volver a lanzar los schedulers en el ThreadPool
        scheduler_futures.clear(); // por si acaso
        start();
    }
} // RUNTIME
