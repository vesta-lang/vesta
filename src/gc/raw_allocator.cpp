/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */
/**
 * @file raw_allocator.cpp
 * @brief Implementacion del asignador de memoria contigua para FFI en VestaVM.
 *
 * Implementa @c RawAllocator::alloc(), @c free(), @c realloc() y @c free_all().
 * Todos los bloques se asignan con @c vm::allocate_memory() (VirtualAlloc/mmap)
 * garantizando rangos contiguos directamente pasables a funciones C nativas.
 */
#include "gc/raw_allocator.h"

#include <cstring>
#include <algorithm>

namespace gc {

    uint64_t RawAllocator::alloc(size_t size) {
        if (size == 0) return 0; /* tama~no cero: no asignar */

        /* reservar memoria contigua con permisos READ|WRITE */
        void *ptr = vm::allocate_memory(size, vm::MemPerm::READ | vm::MemPerm::WRITE);
        if (!ptr) return 0; /* fallo la asignacion del sistema operativo */

        std::memset(ptr, 0, size); /* inicializar a cero para comportamiento predecible */

        uint64_t key      = reinterpret_cast<uint64_t>(ptr); /* clave = direccion host */
        allocations_[key] = { ptr, size };
        total_bytes_     += size;

        /* actualizar estadisticas sin ramas adicionales */
        stats_.alloc_count++;
        stats_.alloc_bytes += size;
        if (total_bytes_ > stats_.peak_bytes)
            stats_.peak_bytes = total_bytes_; /* actualizar high-water mark */

        return key;
    }

    bool RawAllocator::free(uint64_t ptr) {
        auto it = allocations_.find(ptr);
        if (it == allocations_.end()) return false; /* puntero desconocido: no hacer nada */

        stats_.free_count++;
        stats_.freed_bytes += it->second.size;

        vm::free_memory(it->second.host_ptr, it->second.size); /* devolver al sistema */
        total_bytes_ -= it->second.size;
        allocations_.erase(it);
        return true;
    }

    uint64_t RawAllocator::realloc(uint64_t ptr, size_t new_size) {
        stats_.realloc_count++; /* contar siempre, incluso el caso free-por-realloc */

        /* si new_size==0 equivale a free() */
        if (new_size == 0) {
            free(ptr);
            return 0;
        }

        auto it = allocations_.find(ptr);
        if (it == allocations_.end())
            return alloc(new_size); /* ptr invalido: comportamiento como alloc */

        size_t old_size = it->second.size;

        /* asignar nuevo bloque del tamano solicitado */
        void *new_ptr = vm::allocate_memory(new_size, vm::MemPerm::READ | vm::MemPerm::WRITE);
        if (!new_ptr) return 0; /* fallo: mantener bloque original intacto */

        /* copiar min(old_size, new_size) bytes al nuevo bloque */
        std::memcpy(new_ptr, it->second.host_ptr, std::min(old_size, new_size));
        /* rellenar con ceros la zona adicional si el bloque crece */
        if (new_size > old_size)
            std::memset(static_cast<uint8_t *>(new_ptr) + old_size, 0, new_size - old_size);

        /* actualizar estadisticas de la liberacion del bloque viejo */
        stats_.free_count++;
        stats_.freed_bytes += old_size;

        vm::free_memory(it->second.host_ptr, old_size); /* liberar bloque original */
        total_bytes_ -= old_size;
        allocations_.erase(it);

        /* registrar el nuevo bloque */
        uint64_t new_key      = reinterpret_cast<uint64_t>(new_ptr);
        allocations_[new_key] = { new_ptr, new_size };
        total_bytes_         += new_size;

        stats_.alloc_count++;
        stats_.alloc_bytes += new_size;
        if (total_bytes_ > stats_.peak_bytes)
            stats_.peak_bytes = total_bytes_;

        return new_key;
    }

    void RawAllocator::free_all() {
        /* liberar cada bloque registrado y actualizar estadisticas */
        for (auto &[key, rec] : allocations_) {
            stats_.free_count++;
            stats_.freed_bytes += rec.size;
            vm::free_memory(rec.host_ptr, rec.size);
        }
        allocations_.clear();
        total_bytes_ = 0; /* contador de bytes vivos en cero */
    }

} // namespace gc
