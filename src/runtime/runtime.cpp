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

            time_sleep = 0;
            pending_call = nullptr;
        }
    }

    std::string VM::to_string() const {
        return vm_summary(this);
    }

    void VM::print() const {
        // Reutiliza to_string() para evitar duplicar lógica
        vesta::scout() << to_string();
    }
} // RUNTIME
