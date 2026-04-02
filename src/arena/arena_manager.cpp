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

#include "arena/arena_manager.h"

#include <iomanip>
#include <vector>

namespace vm {
    std::string MappedPtr::to_string() const {
        std::ostringstream ss;
        ss << "MappedPtrHost {\n";
        ss << "  Virtual:  0x" << std::hex << std::setw(16) << std::setfill('0')
                << static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ptr_vm.raw)) << std::dec << "\n";

        // mapped.ptr_vm.raw puede ser un vm_map_ptr;
        ss << "  Mapped:   0x" << std::hex << std::setw(16) << std::setfill('0')
                << static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mapped.ptr_vm.raw)) << std::dec << "\n";

        ss << "  Host:     0x" << std::hex << std::setw(16) << std::setfill('0')
                << static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mapped.ptr_host)) << std::dec << "\n";
        ss << "}\n";
        return ss.str();
    }

    void MappedPtr::print(std::ostream &os) const {
        os << to_string();
    }

    ArenaManager::ArenaManager()
        : total_allocated_bytes_(0),
          next_id(0), // Contador cero
          arenas() // unordered_map vacía
    {
    }

    ArenaManager::~ArenaManager() {
        free_all();
    }


    uint64_t ArenaManager::create_arena(size_t size, MemPerm perms) {
        void *mem = allocate_memory(size, perms);
        if (!mem) {
            std::cerr << "[ArenaManager] Error al asignar memoria\n";
            return 0;
        }

        uint64_t id = next_id++;
        arenas[id] = Arena{mem, size, perms};
        total_allocated_bytes_ += size; // +4KB
        return id;
    }

    bool ArenaManager::free_arena(uint64_t id) {
        auto it = arenas.find(id);
        if (it == arenas.end())
            return false;

        total_allocated_bytes_ -= it->second.size; // -4KB
        free_memory(it->second.ptr, it->second.size);
        arenas.erase(it);
        return true;
    }

    const Arena *ArenaManager::get_arena(uint64_t id) const {
        auto it = arenas.find(id);
        if (it == arenas.end()) return nullptr;
        return &it->second;
    }

    void ArenaManager::free_all() {
        // Copia claves para evitar invalidación
        std::vector<int> ids;
        if (arenas.empty()) return;

        for (const auto &[id, arena]: arenas) {
            ids.push_back(id);
        }

        for (int id: ids) {
            free_arena(id);
        }
    }

    // debebo añadir un sistema de cacheado a funciones de este tipo, que su coste es O(N)
    int ArenaManager::find_arena_id_for_ptr(void *host_ptr) {
        for (const auto &[id, arena]: this->arenas) {
            if (arena.ptr == host_ptr) {
                return static_cast<int>(id); // Cast para compatibilidad
            }
        }
        return -1;
    }
}
