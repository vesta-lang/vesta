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

#include "arena/arena.h"

vm::ArenaManager::~ArenaManager() {
    free_all();
}

int vm::ArenaManager::create_arena(size_t size, vm::MemPerm perms) {
    void *mem = allocate_memory(size, perms);
    if (!mem) {
        std::cerr << "[ArenaManager] Error al asignar memoria\n";
        return 0;
    }

    int id     = next_id++;
    arenas[id] = Arena{mem, size, perms};
    total_allocated_bytes_ += size;  // +4KB
    return id;
}

bool vm::ArenaManager::free_arena(int id) {
    auto it = arenas.find(id);
    if (it == arenas.end())
        return false;

    total_allocated_bytes_ -= it->second.size;  // -4KB
    vm::free_memory(it->second.ptr, it->second.size);
    arenas.erase(it);
    return true;
}

const vm::Arena *vm::ArenaManager::get_arena(int id) const {
    auto it = arenas.find(id);
    if (it == arenas.end()) return nullptr;
    return &it->second;
}

void vm::ArenaManager::free_all() {
    for (auto &[id, arena]: arenas) {
        vm::free_memory(arena.ptr, arena.size);
    }
    arenas.clear();
}
