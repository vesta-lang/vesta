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
#include "cli/cli_init_manager_and_server.h"

/**
 * Añadir la nueva VM al manager CLI
 * @param vm manager de instancias inicializado, acepta por valor y lo mueve dentro del vector (por seguridad)
 * @return posicion del manager de instancias en el manager CLI
 */
runtime::ManageVM* ManagerOfManagersAndServer::add_manager(const std::string& name, runtime::ManagerTCPListener *listener) {
    std::lock_guard lk(mtx);
    size_t id = next_id++;
    auto vm = std::make_unique<runtime::ManageVM>(listener, id);
    vm->name_manager = name;

    runtime::ManageVM *raw = vm.get();
    managers_map.emplace(id, std::move(vm));
    return raw;
}

std::vector<std::pair<size_t, ManageVMSnapshot> > ManagerOfManagersAndServer::snapshot() {
    std::lock_guard lk(mtx);

    std::vector<std::pair<size_t, ManageVMSnapshot> > out;
    out.reserve(managers_map.size());

    for (const auto &[id, mgr]: managers_map) {
        ManageVMSnapshot snap{
            mgr->name_manager,
            mgr->vm_count(),
            mgr->listener != nullptr
        };
        out.emplace_back(id, std::move(snap));
    }

    return out;
}


size_t ManagerOfManagersAndServer::size_managers() const {
    std::lock_guard lk(mtx);
    return managers_map.size();
}

std::vector<size_t> ManagerOfManagersAndServer::list_ids() const {
    std::lock_guard lk(mtx);
    std::vector<size_t> ids;
    ids.reserve(managers_map.size());
    for (const auto &p: managers_map) ids.push_back(p.first);
    return ids;
}

std::optional<ManageVMSnapshot> ManagerOfManagersAndServer::get_manager_copy(size_t id) const {
    std::lock_guard lk(mtx);

    auto it = managers_map.find(id);
    if (it == managers_map.end()) return std::nullopt;

    const runtime::ManageVM &mgr = *it->second;

    return ManageVMSnapshot{
        mgr.name_manager,
        mgr.vm_count(),
        mgr.listener != nullptr
    };
}

bool ManagerOfManagersAndServer::replace_manager(size_t id, std::unique_ptr<runtime::ManageVM> vm) {
    std::lock_guard lk(mtx);
    auto it = managers_map.find(id);
    if (it == managers_map.end()) return false;

    it->second = std::move(vm);
    return true;
}
