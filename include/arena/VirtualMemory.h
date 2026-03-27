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

#ifndef VIRTUALMEMORY_H
#define VIRTUALMEMORY_H
#include <cstdint>

#include "arena_manager.h"
#include "TLB.h"

namespace vm {
    class VirtualMemory {
    private:
        tlb::LazyHybridTLB &tlb;
        ArenaManager &      arena_mgr;
        MemPerm             permsDefault;

    public:
        VirtualMemory(tlb::LazyHybridTLB &tlb_, vm::ArenaManager &arena)
            : tlb(tlb_), arena_mgr(arena), permsDefault(MemPerm::EXEC | MemPerm::READ | MemPerm::WRITE) {}

        /**
         * Permite crear un bloque de memoria plana grande
         */
        vm_map_ptr map(uint64_t vaddr, size_t size, MemPerm perms);

        void vm_to_host_memcpy(uint64_t dest_vaddr, const void *src_host, size_t size);

        void vm_to_host_memset(uint64_t dest_vaddr, int value, size_t size);

        void unmap(uint64_t vaddr, size_t size);

        // Acceso directo PLANO (transparente)
        uint8_t &operator[](uint64_t vaddr) {
            tlb::TLBEntryData *entry = tlb.get_entry(vaddr);
            if (!entry) {
                // Lazy allocation automática
                size_t       page_size  = 4096;
                uint64_t     page_vaddr = vaddr & ~0xFFFULL;
                void *       host_mem   = get_ptr_arena(
                        arena_mgr, arena_mgr.create_arena(page_size, permsDefault));

                ptr_mapped pm{};
                pm.ptr_host = host_mem;
                tlb.translate(page_vaddr, MAPPED_PTR_HOST, pm);
                entry = tlb.get_entry(vaddr);
            }
            uint8_t* base = static_cast<uint8_t*>(entry->address.ptr_host);
            return base[vaddr & 0xFFF];
        }
    };
}

#endif //VIRTUALMEMORY_H
