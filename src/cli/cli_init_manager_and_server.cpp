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
size_t ManagerOfManagersAndServer::add_manager(const runtime::ManageVM vm) {
    size_t id = next_id.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lk(mtx);
    managers_map.emplace(id, std::move(vm));
    return id;
}

std::vector<std::pair<size_t, runtime::ManageVM> > ManagerOfManagersAndServer::snapshot() const {
    std::lock_guard lk(mtx);
    std::vector<std::pair<size_t, runtime::ManageVM> > out;
    out.reserve(managers_map.size());
    for (const auto &p: managers_map) out.emplace_back(p.first, p.second); // copia
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

/**
     * @brief Devuelve una copia del manager si existe.
     * @param id Id del manager.
     * @return std::optional con la copia; std::nullopt si no existe.
     */
std::optional<runtime::ManageVM> ManagerOfManagersAndServer::get_manager_copy(size_t id) const {
    std::lock_guard lk(mtx);
    auto it = managers_map.find(id);
    if (it == managers_map.end()) return std::nullopt;
    return it->second; // copia
}

bool ManagerOfManagersAndServer::replace_manager(size_t id, runtime::ManageVM vm) {
    std::lock_guard lk(mtx);
    auto it = managers_map.find(id);
    if (it == managers_map.end()) return false;
    // erase + emplace para forzar construcción desde vm (move-construct)
    managers_map.erase(it);
    managers_map.emplace(id, std::move(vm));
    return true;
}
