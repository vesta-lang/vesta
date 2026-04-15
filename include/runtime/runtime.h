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
#ifndef RUNTIME_H
#define RUNTIME_H
#include <atomic>
#include <cstdint>
#include <functional>
#include <queue>
#include <thread>
#include <vector>

#include "runtime/vm_state_event.h"
#include "decode_instruction.h"
#include "proceso_runtime.h"
#include "scheduler.h"
#include "arena/arena.h"
#include "arena/VirtualMemory.h"
#include "cli/sync_io.h"
#include "runtime/rflags.h"
#include "util/ThreadPool.h"

#include "runtime/pid.h"

#define VERSION_VM 0

namespace loader {
    class Loader;
}

namespace runtime {
    class ManageVM;
    class VM;
    class Scheduler;
    class ProcessVM;

    /**
     * Esta clase representa una instancia de VM.
     * Cada instancia de VM usa un hilo real para ejecutar el codigo dado.
     * Por cada instancia de VM tendremos un hilo por tanto tendremos tantos
     * hilos como instancias, ademas de un hilo principal que sera el que envie y reciba
     * datos a maquina externas e internas.
     *
     * El hilo principal puede crear nuevas instancias o el usuario podra crear nuevas
     */
    class VM {
    public:
        /**
         * Pool de hilos para los gestores de procesos
         */
        ThreadPool pool;

        /**
         * Siguiente gestor de procesos, los procesos se reparten de forma
         * igualitaria entre todos los gestores de procesos.
         */
        size_t next_sched = 0;

        /**
         * Gestores de procesos de la instancia de la VM
         */
        std::vector<std::unique_ptr<Scheduler> > schedulers;

        /**
         * Futures de cada gestor de procesos, se generar al llamar
         * a start junto a los valores del vector schedulers
         */
        std::vector<std::future<void> > scheduler_futures;

        /**
         * Manager de memoria "publico" del manager de instancias
         */
        vm::ArenaManager &manager_mem_public;

        /**
         * Cada instancia de loader permite manejar sus propias cargas
         */
        std::unique_ptr<loader::Loader> loader_priv;

        /**
         * Loader "publico" del manager de instancias.
         */
        loader::Loader &loader_public;

        uint64_t id;

        /**
         * Cada instancia tiene asignada un manager general de instancias
         */
        ManageVM &mgr_vm;

        VM(ManageVM &mgr_vm, uint64_t id_vm);

        [[nodiscard]] std::string to_string() const;

        /**
         * @brief Imprime estado completo de la VM (debug)
         */
        void print();

        /**
         * @brief Inicia la máquina virtual y lanza los schedulers.
         *
         * Crea y configura los `num_schedulers` planificadores (Scheduler),
         * asignándolos al ThreadPool interno para que comiencen a ejecutar
         * procesos de forma concurrente.
         *
         * Este metodo NO bloquea. Para esperar a que la VM finalice,
         * debe llamarse posteriormente a `wait()`.
         *
         * @param num_schedulers Número de schedulers a crear y lanzar.
         */
        void start(size_t num_schedulers);

        /**
         * @brief Obtiene un puntero al proceso asociado a un PID global.
         *
         * El PID global contiene:
         *   - `scheduler_id`: identifica el scheduler propietario del proceso.
         *   - `local_pid`: identifica el proceso dentro de ese scheduler.
         *
         * Este metodo localiza el scheduler correspondiente y devuelve
         * el proceso asociado. No realiza comprobaciones de validez del PID.
         *
         * @param pid PID global del proceso.
         * @return Puntero al proceso, o nullptr si no existe.
         */
        ProcessVM *get_process(GlobalPID pid);

        /**
         * @brief Bloquea el hilo que creó la VM hasta que todos los schedulers finalicen.
         *
         * Este mwtodo espera a que todas las tareas asociadas a los schedulers
         * hayan terminado su ejecución. La VM puede finalizar por:
         *
         * Este mwtodo es el equivalente a `join()` en un sistema de hilos.
         */
        void wait();

        /**
         * @brief Marca un proceso como listo para ejecutarse.
         *
         * Inserta el PID en la cola de procesos listos del scheduler correspondiente.
         * Esto permite que el proceso sea seleccionado por el planificador en la
         * siguiente iteración del run loop.
         *
         * @param pid PID global del proceso que debe pasar a estado READY.
         */
        void make_ready(GlobalPID pid);


        /**
         * Permite crear un nuevo proceso en un gestor de procesos.
         * @return PID del proceso creado en ese gestro de procesos.
         */
        GlobalPID spawn_process();

        std::string vm_summary() const {
            std::ostringstream ss;

            ss << "ID=" << vesta::hex64((uint64_t) id) << "\n";

            return ss.str();
        }

    private:
        /**
         * Mutex unico para cada instancia de VM. Con esto tenemos seguridad de que solo un hilo
         * modifique el estado a la vez.
         */
        std::mutex state_lock;
    };


    static void dump_vm_region(tlb::LazyHybridTLB *tlb, uint64_t vaddr, size_t size) {
        uint64_t start = vaddr & ~0xFFFULL; // Alinear a página
        uint64_t end   = (vaddr + size + 0xFFFULL) & ~0xFFFULL;


        vesta::scout() << "==================== VM Memory Dump ====================\n";
        vesta::scout() << "Virtual region: 0x" << std::hex << start
                << " - 0x" << end
                << "  (" << std::dec << size << " bytes)\n\n";

        for (uint64_t page = start; page < end; page += 0x1000) {
            void *host = tlb->get_real_host_ptr_of_vptr(page);

            if (!host) {
                vesta::scout() << "  [0x" << std::hex << page << "]  ->  <not mapped>\n";
                continue;
            }

            vesta::scout() << "  [0x" << std::hex << page << "]  ->  host=" << host << "\n";

            // Mostrar primeros 16 bytes en formato hexdump
            const auto *p = static_cast<uint8_t *>(host);

            vesta::scout() << "       data: ";

            vesta::scout() << vesta::dump(p, size);

            vesta::scout() << "\n";
        }

        vesta::scout() << "================== End VM Memory Dump ==================\n";
    }
}

#endif //RUNTIME_H
