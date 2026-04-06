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
        ArenaManager &arena_mgr;
        MemPerm permsDefault = MemPerm::NONE;

    public:
        VirtualMemory(tlb::LazyHybridTLB &tlb_, vm::ArenaManager &arena)
            : tlb(tlb_), arena_mgr(arena), permsDefault(MemPerm::EXEC | MemPerm::READ | MemPerm::WRITE) {
        }

        /**
         * Permite crear un bloque de memoria plana grande
         */
        vm_map_ptr map(uint64_t vaddr, size_t size, MemPerm perms);

        void vm_to_host_memcpy(uint64_t dest_vaddr, const void *src_host, size_t size);

        void vm_to_host_memset(uint64_t dest_vaddr, int value, size_t size);

        /**
         * Lee un bloque de memoria virtual y lo copia en un buffer destino.
         *
         * Esta función permite realizar lecturas multibyte eficientes sobre memoria
         * virtual, gestionando automáticamente la traducción mediante la TLB y
         * resolviendo saltos de página cuando el rango solicitado abarca más de una
         * página de 4 KiB.
         *
         * Si una dirección virtual no está presente en la TLB y el tipo de mapeo lo
         * permite (MAPPED_PTR_HOST o NONE), se realiza una asignación perezosa
         * (lazy allocation) creando una nueva página en el arena manager y
         * registrándola en la TLB. Para otros tipos de mapeo, la función delega en la
         * lógica correspondiente o genera un error si el modo no está implementado.
         *
         * Optimización:
         *  - Cuando el puntero físico resultante está alineado a 16 bytes, se activa
         *    un *fast‑path* que permite al compilador generar accesos alineados
         *    mediante `__builtin_assume_aligned()`. Esto habilita instrucciones SIMD
         *    alineadas (movaps/vmovaps o equivalentes NEON/AVX), reduciendo el coste
         *    de copia en bloques grandes. Si la alineación no está garantizada, la
         *    función recurre automáticamente al camino seguro tradicional.
         *
         * Esta optimización es transparente para el usuario y mantiene compatibilidad
         * total con la versión anterior.
         *
         * @param vaddr Dirección virtual inicial desde la que se desea leer.
         * @param dst   Puntero al buffer destino donde se almacenarán los bytes leídos.
         * @param size  Número de bytes a leer desde memoria virtual.
         */
        void read_bytes(uint64_t vaddr, void *dst, size_t size);

        uint32_t read_u16(uint64_t vaddr);

        /**
         * Lee un valor de 32 bits desde memoria virtual.
         *
         * Esta función es un atajo sobre `read_bytes()` para obtener un entero
         * de 32 bits desde la dirección virtual indicada. Gestiona automáticamente
         * la traducción mediante TLB y los posibles saltos de página.
         *
         * @param vaddr Dirección virtual desde la que se desea leer el valor.
         * @return Valor de 32 bits leído desde memoria virtual.
         */
        uint32_t read_u32(uint64_t vaddr);

        /**
         * Lee un valor de 64 bits desde memoria virtual.
         *
         * Esta función es un atajo sobre `read_bytes()` para obtener un entero
         * de 64 bits desde la dirección virtual indicada. Gestiona automáticamente
         * la traducción mediante TLB y los posibles saltos de página.
         *
         * @param vaddr Dirección virtual desde la que se desea leer el valor.
         * @return Valor de 64 bits leído desde memoria virtual.
         */
        uint64_t read_u64(uint64_t vaddr);


        void unmap(uint64_t vaddr, size_t size);

        uint8_t &operator[](uint64_t vaddr) {
            tlb::TLBEntryData *entry = tlb.get_entry(vaddr);

            if (!entry || entry->type_address != MAPPED_PTR_HOST) {
                uint8_t dummy;
                read_bytes(vaddr, &dummy, 1);
                entry = tlb.get_entry(vaddr);
            }

            uint8_t *base = static_cast<uint8_t *>(entry->address.ptr_host);
            return base[vaddr & 0xFFF];
        }
    };
}

#endif //VIRTUALMEMORY_H
