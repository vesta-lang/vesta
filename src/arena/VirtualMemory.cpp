#include "arena/VirtualMemory.h"

void vm_memcpy() {
}

namespace vm {
    vm_map_ptr VirtualMemory::map(uint64_t vaddr, size_t size, vm::MemPerm perms) {
        // Alinear inicio y fin
        uint64_t start = vaddr & ~0xFFFULL;
        uint64_t end   = (vaddr + size + 0xFFFULL) & ~0xFFFULL;
        size_t total_size = end - start;

        // Crear UNA arena para toda la región
        uint64_t id = arena_mgr.create_arena(total_size, perms);
        const Arena* arena = arena_mgr.get_arena(id);

        // Mapear cada página virtual a su offset dentro de la arena
        for (uint64_t page = start; page < end; page += 0x1000) {
            ptr_mapped pm{};
            pm.ptr_host = (uint8_t*)arena->ptr + (page - start);
            tlb.translate(page, MAPPED_PTR_HOST, pm);
        }

        vm_map_ptr result{};
        result.raw = start;
        return result;
    }


    void VirtualMemory::unmap(uint64_t vaddr, size_t size) {
        uint64_t end = vaddr + size;

        for (uint64_t page = vaddr & ~0xFFFULL; page < end; page += 0x1000) {
            // Obtener entrada TLB
            tlb::TLBEntryData *entry = tlb.get_entry(page);
            if (!entry || entry->type_address != vm::MAPPED_PTR_HOST) {
                continue; // Página no mapeada/host
            }

            // Liberar arena asociada
            int arena_id = arena_mgr.find_arena_id_for_ptr(entry->address.ptr_host);
            if (arena_id != -1) {
                arena_mgr.free_arena(arena_id);
            }

            // Eliminar entrada TLB (marcar inválida)
            tlb.clear_tlb_entry(page);
        }
    }


    void VirtualMemory::vm_to_host_memcpy(uint64_t dest_vaddr,
                                          const void *src_host, size_t size) {
        const uint8_t *src = (const uint8_t *) src_host;
        uint64_t vaddr = dest_vaddr & ~0xFFFULL; // Primera página

        while (size > 0) {
            // Obtener HOST real de página virtual
            void *host_dest = this->tlb.get_real_host_ptr_of_vptr(vaddr);
            if (!host_dest) {
                // Auto-mapear página si no existe
                this->map(vaddr, 4096, vm::MemPerm::READ | vm::MemPerm::WRITE);
                host_dest = this->tlb.get_real_host_ptr_of_vptr(vaddr);
            }

            // Calcular bytes a copiar en esta página
            size_t offset = dest_vaddr & 0xFFF;
            size_t page_avail = 4096 - offset;
            size_t copy_size = std::min(size, page_avail);

            // MEMCPY REAL página a página
            memcpy((uint8_t *) host_dest + offset, src, copy_size);

            // Siguiente página
            src += copy_size;
            size -= copy_size;
            dest_vaddr += copy_size;
            vaddr = dest_vaddr & ~0xFFFULL;
        }
    }

    void VirtualMemory::vm_to_host_memset(uint64_t dest_vaddr, int value, size_t size) {
        uint64_t vaddr = dest_vaddr & ~0xFFFULL;

        while (size > 0) {
            // Obtener HOST real de página virtual
            void *host_dest = tlb.get_real_host_ptr_of_vptr(vaddr);
            if (!host_dest) {
                // Auto-mapear página si no existe (RW)
                map(vaddr, 4096, vm::MemPerm::READ | vm::MemPerm::WRITE);
                host_dest = tlb.get_real_host_ptr_of_vptr(vaddr);
            }

            // Calcular bytes a llenar en esta página
            size_t offset = dest_vaddr & 0xFFF;
            size_t page_avail = 4096 - offset;
            size_t fill_size = std::min(size, page_avail);

            // MEMSET REAL página a página
            memset((uint8_t *) host_dest + offset, value, fill_size);

            // Siguiente página
            size -= fill_size;
            dest_vaddr += fill_size;
            vaddr = dest_vaddr & ~0xFFFULL;
        }
    }
}
