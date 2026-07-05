/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */
/**
 * @file cli_init_manager_and_server.cpp
 * @brief Implementacion de @c ManagerOfManagersAndServer y helpers de
 * inicializacion de VM.
 *
 * Implementa los metodos de @c ManagerOfManagersAndServer: @c add_manager(),
 * @c snapshot(), @c modify_manager<Func>() y @c find_if<Pred>().
 * Tambien contiene las funciones auxiliares para crear y registrar nuevas
 * instancias de VM en el manager CLI.
 */
#include "cli/cli_init_manager_and_server.h"

/**
 * Anadir la nueva VM al manager CLI
 * @param vm manager de instancias inicializado, acepta por valor y lo mueve
 * dentro del vector (por seguridad)
 * @return posicion del manager de instancias en el manager CLI
 */
runtime::ManageVM *
ManagerOfManagersAndServer::add_manager(const std::string &name,
                                        runtime::ManagerTCPListener *listener) {
    std::lock_guard lk(mtx);
    size_t id = next_id++;
    auto vm = std::make_unique<runtime::ManageVM>(listener, id);
    vm->name_manager = name;

    runtime::ManageVM *raw = vm.get();
    managers_map.emplace(id, std::move(vm));
    return raw;
}

std::vector<std::pair<size_t, ManageVMSnapshot>>
ManagerOfManagersAndServer::snapshot() {
    std::lock_guard lk(mtx);

    std::vector<std::pair<size_t, ManageVMSnapshot>> out;
    out.reserve(managers_map.size());

    for (const auto &[id, mgr] : managers_map) {
        ManageVMSnapshot snap{mgr->name_manager, mgr->vm_count(),
                              mgr->listener != nullptr};
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
    for (const auto &p : managers_map)
        ids.push_back(p.first);
    return ids;
}

std::optional<ManageVMSnapshot>
ManagerOfManagersAndServer::get_manager_copy(size_t id) const {
    std::lock_guard lk(mtx);

    auto it = managers_map.find(id);
    if (it == managers_map.end()) return std::nullopt;

    const runtime::ManageVM &mgr = *it->second;

    return ManageVMSnapshot{mgr.name_manager, mgr.vm_count(),
                            mgr.listener != nullptr};
}

bool ManagerOfManagersAndServer::remove_manager(size_t id) {
    std::lock_guard lk(mtx);
    // erase devuelve el numero de elementos eliminados: 1 si existia, 0 si no
    return managers_map.erase(id) > 0;
}

bool ManagerOfManagersAndServer::replace_manager(
    size_t id, std::unique_ptr<runtime::ManageVM> vm) {
    std::lock_guard lk(mtx);
    auto it = managers_map.find(id);
    if (it == managers_map.end()) return false;

    it->second = std::move(vm);
    return true;
}
