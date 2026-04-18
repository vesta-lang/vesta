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

#include "runtime/proceso_runtime.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <queue>
#include <thread>
#include <vector>

#include "arena/arena.h"
#include "arena/VirtualMemory.h"
#include "cli/sync_io.h"
#include "jit/jit_call.h"
#include "profiler/timer.h"
#include "runtime/rflags.h"

namespace loader {
    class Loader;
}

namespace runtime {
    struct InstrFormat;
    class ManageVM;
    class VM;

    PendingCall_t *pending_call = nullptr; /** Llamada nativa en curso, si hay alguna */

    ProcessVM::ProcessVM(Scheduler &scheduler, GlobalPID pid): pid(pid), vm_mem(tlb, manager_mem_priv),
                                                               scheduler(scheduler) {}
    ProcessVM::~ProcessVM() {
        state = DEAD;
        manager_mem_priv.free_all(); // en teoria no es necesario por que ya lo hace el destructor
    }

    void ProcessVM::reset_cache() {
        // resetear icache
        for (auto &entry: icache) {
            entry.pc    = UINT64_MAX; // invalida
            decoded_ptr = nullptr;    // invalidar punteros decoder anteriores.
        }
    }

    void ProcessVM::load_raw_code(uint64_t address, const std::vector<uint8_t> &code) {
        // copiar a memoria virtual
        vm_mem.vm_to_host_memcpy(address, code.data(), code.size());

        // Configurar
        registers.rip.qword(address);

        // No debemos configurar manualmente el estado del proceso,
        // si el proceso es nuevo, el proceso se configura automaticamente
        // en el estado de NEW, este estado es necesario para que el metodo
        // Scheduler::make_ready del gestor de procesos, pueda detectar
        // que el proceso es nuevo, se incremente el contador de procesos
        // vivos que requiere que el proceso este en NEW y cambie el estado
        // del proceso a READY 
    }

    std::string ProcessVM::to_string() const {
        return vm_summary();
    }

    std::string ProcessVM::vm_summary() const {
        std::ostringstream ss;

        ss << "PID LOCAL=" << vesta::hex64((uint64_t) pid.local_pid) <<
                " ID SCHEDULER=" << vesta::hex64((uint64_t) pid.scheduler_id)
                << " st=" << vm_state_to_str(state) << "\n";

        // Registros generales R00–R15
        for (int i = 0; i < 16; ++i) {
            ss << " R" << std::setw(2) << std::setfill('0') << i
                    << "=" << vesta::hex64(registers.regs[i].qword());
            if (i % 2 == 1) ss << "\n";
        }
        ss << "\n";

        // CUR0–CUR3
        for (int i = 0; i < 4; ++i) {
            ss << " CUR" << i << "=" << vesta::hex64(registers.cur[i].qword());
            if (i % 2 == 1) ss << "\n";
        }
        ss << "\n";

        // IP/SP/BP
        ss << " RIP=" << vesta::component_to_string(registers.rip)
                << " RSP=" << vesta::component_to_string(registers.stack_pointer)
                << " RBP=" << vesta::component_to_string(registers.base_pointer)
                << "\n";

        // FLAGS
        ss << " FLAGS=["
                << "CF=" << (int) registers.flags.bits.CF << " "
                << "OF=" << (int) registers.flags.bits.OF << " "
                << "SF=" << (int) registers.flags.bits.SF << " "
                << "ZF=" << (int) registers.flags.bits.ZF << " "
                << "DM=" << (int) registers.flags.bits.DM
                << "]\n";

        ss << " Reductions=" << reductions_remaining
                << " TSC=" << tsc
                << " SleepUntil=" << time_sleep
                << " LastErr=" << err_thread
                << "\n";

        if (decoded_ptr) {
            ss << " Instr: opcode=" << (int) decoded_ptr->flags_info.opcode_index
                    << " size=" << (int) decoded_ptr->flags_info.size_instr
                    << " mode=" << (int) decoded_ptr->flags_info.mode
                    << " did_jump=" << decoded_ptr->flags_info.did_jump
                    << " blocking=" << decoded_ptr->flags_info.blocking
                    << " pc=" << vesta::hex64(decoded_ptr->pc)
                    << "\n";
        }

        size_t valid = 0;
        for (int i = 0; i < ICACHE_SIZE; i++)
            if (icache[i].pc != ~0ull) // entrada válida
                valid++;

        ss << " ICACHE: valid=" << valid << "/" << ICACHE_SIZE
                << " (" << (100 * valid / ICACHE_SIZE) << "%)\n";

        ss << " MEM: total_allocated_bytes=" << manager_mem_priv.total_allocated_bytes_
                << "\n";

        ss << " SchedulerID=" << scheduler.id_scheduler
                << " Ready=" << scheduler.ready_queue.size()
                //<< " Alive=" << scheduler.count_alive()
                << "\n";


        // Thread / sleep
        ss << " Sleep=" << time_sleep
                << "\n";

        return ss.str();
    }
}
