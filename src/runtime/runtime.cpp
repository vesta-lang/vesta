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



namespace runtime {
    VM::VM(
        ManageVM &mgr_vm,
        uint64_t  id_vm
    ): mgr_vm(mgr_vm), // manager de instancias
       manager_mem_public(mgr_vm.manager_mem),
       loader_priv(mgr_vm.manager_mem),
       loader_public(mgr_vm.loader),
       state(BLOCKED),
       err_thread(THREAD_NO_ERROR),
       id({id_vm}) {
        {
            this->manager_mem_priv = {};

            // aun no se cargo nada asi que se inicializara a 0 todo
            stack_pointer = vm::MappedPtrHost{

            };

            base_pointer = vm::MappedPtrHost{

            };

            rip = vm::MappedPtrHost{

            };

            time_sleep   = 0;
            pending_call = nullptr;
        }
    }

    void VM::print() const {
        printf("=== VM ID: 0x%016llu ===\n", id.id);

        // Estado legible
        const char *state_str = state == READY
                                    ? "READY"
                                    : state == RUNNING
                                          ? "RUNNING"
                                          : state == BLOCKED
                                                ? "BLOCKED"
                                                : "DEAD";
        printf("State: %s | Error: %d | TSC: %llu\n",
               state_str, static_cast<int>(err_thread), tsc);

        printf("IP: ");
        rip.print();
        putchar('\n');

        printf("SP: ");
        stack_pointer.print();
        putchar('\n');

        printf("BP: ");
        base_pointer.print();
        putchar('\n');

        // Flags
        printf("Flags: ZF=%u CF=%u OF=%u SF=%u\n",
               flags.bits.ZF, flags.bits.CF, flags.bits.OF, flags.bits.SF);

        printf("R00: 0x%016llx | R01: 0x%016llx | R02: 0x%016llx | R03: 0x%016llx\n",
               r00.qword(), r01.qword(), r02.qword(), r03.qword());
        printf("R04: 0x%016llx | R05: 0x%016llx | R06: 0x%016llx | R07: 0x%016llx\n",
               r04.qword(), r05.qword(), r06.qword(), r07.qword());
        printf("R08: 0x%016llx | R09: 0x%016llx | R10: 0x%016llx | R11: 0x%016llx\n",
               r08.qword(), r09.qword(), r10.qword(), r11.qword());
        printf("R12: 0x%016llx | R13: 0x%016llx | R14: 0x%016llx | R15: 0x%016llx\n",
               r12.qword(), r13.qword(), r14.qword(), r15.qword());

        // Timing/pending
        printf("Sleep: %llu ns | Pending: %p | Thread: %llu\n",
               time_sleep, pending_call, (uint64_t) thread_for_vm);
        printf("========================\n");
    }
} // RUNTIME
