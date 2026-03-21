#include "arena/VirtualMemory.h"

void vm_memcpy() {

}

namespace vm {
    vm_map_ptr VirtualMemory::map(uint64_t vaddr, size_t size, vm::MemPerm perms) {
        uint64_t end = vaddr + size;
        for (uint64_t page = vaddr & ~0xFFFULL; page < end; page += 0x1000) {
            void* host_mem = get_ptr_arena(
                    arena_mgr, arena_mgr.create_arena(page, permsDefault));
            tlb.translate(page, vm::MAPPED_PTR_HOST,
                         vm::ptr_mapped{.ptr_host = host_mem});
        }
        return vm_map_ptr{.raw = (vaddr & ~0xFFFULL)};
    }



    void VirtualMemory::unmap(uint64_t vaddr, size_t size) {
        uint64_t end = vaddr + size;

        for (uint64_t page = vaddr & ~0xFFFULL; page < end; page += 0x1000) {
            // Obtener entrada TLB
            tlb::TLBEntryData* entry = tlb.get_entry(page);
            if (!entry || entry->type_address != vm::MAPPED_PTR_HOST) {
                continue;  // Página no mapeada/host
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
               const void* src_host, size_t size) {
        const uint8_t* src = (const uint8_t*)src_host;
        uint64_t vaddr = dest_vaddr & ~0xFFFULL;  // Primera página

        while (size > 0) {
            // Obtener HOST real de página virtual
            void* host_dest = this->tlb.get_real_host_ptr_of_vptr(vaddr);
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
            memcpy((uint8_t*)host_dest + offset, src, copy_size);

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
            void* host_dest = tlb.get_real_host_ptr_of_vptr(vaddr);
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
            memset((uint8_t*)host_dest + offset, value, fill_size);

            // Siguiente página
            size -= fill_size;
            dest_vaddr += fill_size;
            vaddr = dest_vaddr & ~0xFFFULL;
        }
    }



}