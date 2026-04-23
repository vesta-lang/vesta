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
#include "gc/raw_allocator.h"

#include <cstring>
#include <algorithm>

namespace gc {

    uint64_t RawAllocator::alloc(size_t size) {
        if (size == 0) return 0;

        void *ptr = vm::allocate_memory(size, vm::MemPerm::READ | vm::MemPerm::WRITE);
        if (!ptr) return 0;

        std::memset(ptr, 0, size);

        uint64_t key   = reinterpret_cast<uint64_t>(ptr);
        allocations_[key] = { ptr, size };
        total_bytes_   += size;

        // stats: sin ramas adicionales, solo incrementos
        stats_.alloc_count++;
        stats_.alloc_bytes += size;
        if (total_bytes_ > stats_.peak_bytes) stats_.peak_bytes = total_bytes_;

        return key;
    }

    bool RawAllocator::free(uint64_t ptr) {
        auto it = allocations_.find(ptr);
        if (it == allocations_.end()) return false;

        stats_.free_count++;
        stats_.freed_bytes += it->second.size;

        vm::free_memory(it->second.host_ptr, it->second.size);
        total_bytes_ -= it->second.size;
        allocations_.erase(it);
        return true;
    }

    uint64_t RawAllocator::realloc(uint64_t ptr, size_t new_size) {
        stats_.realloc_count++; // contar siempre, incluso el caso free-por-realloc

        if (new_size == 0) {
            free(ptr);
            return 0;
        }

        auto it = allocations_.find(ptr);
        if (it == allocations_.end())
            return alloc(new_size);

        size_t old_size = it->second.size;

        void *new_ptr = vm::allocate_memory(new_size, vm::MemPerm::READ | vm::MemPerm::WRITE);
        if (!new_ptr) return 0;

        std::memcpy(new_ptr, it->second.host_ptr, std::min(old_size, new_size));
        if (new_size > old_size)
            std::memset(static_cast<uint8_t *>(new_ptr) + old_size, 0, new_size - old_size);

        // stats de la liberacion del bloque viejo
        stats_.free_count++;
        stats_.freed_bytes += old_size;

        vm::free_memory(it->second.host_ptr, old_size);
        total_bytes_ -= old_size;
        allocations_.erase(it);

        uint64_t new_key      = reinterpret_cast<uint64_t>(new_ptr);
        allocations_[new_key] = { new_ptr, new_size };
        total_bytes_         += new_size;

        stats_.alloc_count++;
        stats_.alloc_bytes += new_size;
        if (total_bytes_ > stats_.peak_bytes) stats_.peak_bytes = total_bytes_;

        return new_key;
    }

    void RawAllocator::free_all() {
        for (auto &[key, rec] : allocations_) {
            stats_.free_count++;
            stats_.freed_bytes += rec.size;
            vm::free_memory(rec.host_ptr, rec.size);
        }
        allocations_.clear();
        total_bytes_ = 0;
    }

} // namespace gc
