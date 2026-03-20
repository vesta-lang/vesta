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

ManageVM::~ManageVM() {
    destroy_all_vms();
}

uint64_t ManageVM::create_vm() {
    uint64_t new_id = ++counter_vm;

    VM new_vm;
    new_vm.id.id = new_id;

    vms.emplace_back(std::move(new_vm));
    return new_id;
}

bool ManageVM::destroy_vm(uint64_t vm_id) {
    for (auto it = vms.begin(); it != vms.end(); ++it) {
        if (it->id.id == vm_id) {
            vms.erase(it);
            return true;
        }
    }
    return false;
}

VM &ManageVM::get_vm(uint64_t vm_id) {
    for (auto &vm: vms) {
        if (vm.id.id == vm_id) {
            return vm;
        }
    }
    throw std::runtime_error("VM not found");
}

const VM &ManageVM::get_vm(uint64_t vm_id) const {
    for (const auto &vm: vms) {
        if (vm.id.id == vm_id) {
            return vm;
        }
    }
    throw std::runtime_error("VM not found");
}

bool ManageVM::has_vm(uint64_t vm_id) const {
    for (const auto &vm: vms) {
        if (vm.id.id == vm_id) {
            return true;
        }
    }
    return false;
}

size_t ManageVM::vm_count() const {
    return vms.size();
}

void ManageVM::destroy_all_vms() {
    vms.clear();
    counter_vm = 0;
}
