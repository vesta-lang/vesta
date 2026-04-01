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

#include <algorithm>

#include "runtime/runtime.h"

namespace runtime {
    ManageVM::ManageVM(
        ManagerTCPListener &listener,
        Sqlite::SqliteSingleton &db
    ): listener(listener),
                                                      loader(this->manager_mem), db(db), counter_vm(0) {
    }

    ManageVM::~ManageVM() {
        destroy_all_vms();
    }

    uint64_t ManageVM::create_vm() {
        uint64_t new_id = counter_vm++;

        // cada instancia tiene un manager publica de instancia y de carga
        VM *vm = new VM(*this, new_id);
        vms.emplace_back(vm);
        return new_id;
    }

    bool ManageVM::destroy_vm(uint64_t vm_id) {
        for (auto it = vms.begin(); it != vms.end(); ++it) {
            if ((*it)->id.id == vm_id) {
                delete *it;       // Liberar memoria
                vms.erase(it);    // QUITAR de vector
                return true;
            }
        }
        return false;
    }



    VM *ManageVM::get_vm(uint64_t vm_id) {
        for (auto ptr: vms) {
            if (ptr->id.id == vm_id) {
                return ptr;
            }
        }
        return nullptr;
    }


    bool ManageVM::has_vm(uint64_t vm_id) const {
        for (const auto &vm: vms) {
            if (vm->id.id == vm_id) {
                return true;
            }
        }
        return false;
    }

    size_t ManageVM::vm_count() const {
        return vms.size();
    }

    void ManageVM::destroy_all_vms() {
        for (auto *vm: vms) {
            if (vm != nullptr) {
                printf("[DEBUG] Deleting VM ID=%llu\n", vm->id.id);
                destroy_vm(vm->id.id);
                vm = nullptr;
            }
        }
        vms.clear();
        counter_vm = 0;
    }
}
