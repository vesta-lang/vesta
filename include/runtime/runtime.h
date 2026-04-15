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
#include "profiler/timer.h"
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





        pthread_t thread_for_vm{};
        uint64_t  id;

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

        void start(size_t num_schedulers);

        /**
         * Permite hacer que el hilo que creo la VM espere a la finalizacion
         * de la VM a traves de algun error, la instruccion HLT u otro evento
         * o motivo que desencadene una finalizacion.
         */
        //void join();

        ProcessVM *get_process(GlobalPID pid);

        void make_ready(GlobalPID pid);



        /**
         * Permite crear un nuevo proceso en un gestor de procesos.
         * @return PID del proceso creado en ese gestro de procesos.
         */
        GlobalPID spawn_process();

        /*std::string vm_summary() const {
            std::ostringstream ss;

            ss << "ID=" << vesta::hex64((uint64_t) id.id)
                    << " st=" << vm_state_to_str(state) << "\n";

            // Registros generales R00–R15
            for (int i = 0; i < 16; ++i) {
                ss << " R" << std::setw(2) << std::setfill('0') << i
                        << "=" << vesta::hex64(regs[i].qword());
                if (i % 2 == 1) ss << "\n";
            }
            ss << "\n";

            // CUR0–CUR3
            for (int i = 0; i < 4; ++i) {
                ss << " CUR" << i << "=" << vesta::hex64(cur[i].qword());
                if (i % 2 == 1) ss << "\n";
            }
            ss << "\n";

            // IP/SP/BP
            ss << " RIP=" << vesta::component_to_string(rip)
                    << " RSP=" << vesta::component_to_string(stack_pointer)
                    << " RBP=" << vesta::component_to_string(base_pointer)
                    << "\n";

            // FLAGS
            ss << " FLAGS=["
                    << "CF=" << (int) flags.bits.CF << " "
                    << "OF=" << (int) flags.bits.OF << " "
                    << "SF=" << (int) flags.bits.SF << " "
                    << "ZF=" << (int) flags.bits.ZF << " "
                    << "DM=" << (int) flags.bits.DM
                    << "]\n";

            // Thread / sleep
            ss << " Th=" << (void *) thread_for_vm
                    << " Sleep=" << time_sleep;

            return ss.str();
        }*/



        /**
         * Ejecuta la instrucción actualmente decodificada y devuelve el evento
         * que debe procesar la máquina virtual como resultado de dicha ejecución.
         *
         * En lugar de un valor booleano, este metodo devuelve directamente un
         * vm_event que representa la transición que debe realizar la FSM.
         *
         * Esto permite que una instrucción genere múltiples tipos de eventos:
         *  - EVT_EXEC_DONE  -> La instrucción terminó correctamente.
         *  - EVT_IO_WAIT    -> La instrucción es bloqueante y requiere esperar E/S.
         *  - EVT_HALT       -> La instrucción solicita detener la VM.
         *  - EVT_ERROR      -> Se produjo un error fatal durante la ejecución.
         *  - Otros eventos específicos según la arquitectura de la VM.
         *
         * El metodo también se encarga de avanzar el contador de programa (PC)
         * si la instrucción no ha modificado explícitamente su valor (campo did_jump).
         *
         * @return vm_event  Evento que la FSM debe procesar tras ejecutar la instrucción.
         */
        vm_event execute_instruction();




        /**
         * permite indicar que se puede seguir ejecutado las funcionalidades
         * de "profiler" internas o externas a la VM.
         */
        std::atomic<bool> profiler_running = true;

    private:
        /**
         * Mutex unico para cada instancia de VM. Con esto tenemos seguridad de que solo un hilo
         * modifique el estado a la vez.
         */
        std::mutex state_lock;
    };

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
    /*static void profiler_thread(ProcessVM *vm) {
        uint64_t last = 0;

        // mientras la vm no deba haber acabado
        while (vm->scheduler.vm_reference.profiler_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // --- IPS ---
            uint64_t now   = vm->scheduler.vm_reference.profiler_instr_counter;
            uint64_t delta = now - last;
            last           = now;

            uint64_t ips = delta * 256;

            // --- CPU ---
            double cpu    = (vm->scheduler.vm_reference.time_exec / 1e9) * 100.0;
            vm->scheduler.vm_reference.time_exec = 0;

            // --- Salida thread-safe ---
            vesta::scout()
                    << "\r[VM " << vesta::hex64(vm->scheduler.vm_reference.id) << "] "
                    << "IPS: " << ips
                    << " | CPU: " << cpu << "%\n";
        }
    }*/

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
