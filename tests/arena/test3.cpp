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

#include <iostream>
#include <cassert>
#include <iomanip>
#include "arena/arena.h"
#include "arena/TLB.h"


int main() {
    tlb::LazyHybridTLB lazy{};
    for (uint64_t i = 0x1000; i < 0xfffffff; i+=0x10000 + (i % 0x1000)) {
        lazy.translate(
                i,
                vm::MAPPED_PTR_VM,
                vm::ptr_mapped{.ptr_host = reinterpret_cast<vm::host_ptr>(i)}
                );


        std::cout << lazy.get_entry(i)->to_string()
            << std::endl;

        lazy.dump_stats();
    }

    vm::vm_map_ptr val = {.raw = 0xfff1000};
    val.set(vm::PageLevel::PT2, 0x12);
    val.set(vm::PageLevel::OFFSET, 0x123);
    lazy.dump_tree(val.raw);

    return 0;
}