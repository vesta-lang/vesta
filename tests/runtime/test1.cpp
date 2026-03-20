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

int main() {
    ManageVM manager = ManageVM();

    uint64_t vm1_id = manager.create_vm(); // ID=1
    uint64_t vm2_id = manager.create_vm(); // ID=2

    VM &vm1 = manager.get_vm(vm1_id);
    printf("VM %llu tiene %zu maquinas\n", vm1_id, manager.vm_count());

    manager.destroy_vm(vm1_id);
    bool exists = manager.has_vm(vm1_id); // false

    return 0;
}
