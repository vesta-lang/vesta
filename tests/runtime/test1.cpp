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

#include "runtime/manager_runtime.h"
#include <iostream>
#include <pthread.h>
#include <cstdio>  // printf

#include "runtime/runtime.h"

int main() {
    runtime::ManageVM manager = runtime::ManageVM();

    uint64_t vm1_id = manager.create_vm(); // ID=1
    uint64_t vm2_id = manager.create_vm(); // ID=2

    printf("VM Manager %llu tiene %zu maquinas\n", vm1_id, manager.vm_count());

    for (int i = 0; i < manager.vm_count(); i++) {
        if (auto vm = manager.get_vm(i)) {
            vm->print();
            printf("VMs: %zu | Arenas VM1: %zu\n", manager.vms.size(), vm->manager_mem_priv.arenas.size());

        } else {
            printf("VM no encontrada\n");
        }

    }

    manager.destroy_vm(vm1_id);
    bool exists = manager.has_vm(vm1_id); // false
    printf("VM %llu existe: %s\n", vm1_id, exists ? "SI" : "NO");

    return 0;
}
