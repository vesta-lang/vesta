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

#include <vector>

namespace vm {
    ArenaManager::ArenaManager()
        : arenas(),                 // unordered_map vacía
          total_allocated_bytes_(0) // Contador cero
    {}

    ArenaManager::~ArenaManager() {
        free_all();
    }

    int ArenaManager::create_arena(size_t size, MemPerm perms) {
        void *mem = allocate_memory(size, perms);
        if (!mem) {
            std::cerr << "[ArenaManager] Error al asignar memoria\n";
            return 0;
        }

        int id     = next_id++;
        arenas[id] = Arena{mem, size, perms};
        total_allocated_bytes_ += size; // +4KB
        return id;
    }

    bool ArenaManager::free_arena(int id) {
        auto it = arenas.find(id);
        if (it == arenas.end())
            return false;

        total_allocated_bytes_ -= it->second.size; // -4KB
        free_memory(it->second.ptr, it->second.size);
        arenas.erase(it);
        return true;
    }

    const Arena *ArenaManager::get_arena(int id) const {
        auto it = arenas.find(id);
        if (it == arenas.end()) return nullptr;
        return &it->second;
    }

    void ArenaManager::free_all() {
        // Copia claves para evitar invalidación
        std::vector<int> ids;
        for (const auto &[id, arena]: arenas) {
            ids.push_back(id);
        }

        for (int id: ids) {
            free_arena(id);
        }
    }
}
