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

#include "arena/TLB.h"

#include "arena/arena.h"


namespace tlb {
    inline TLBEntry &ensure_level(std::vector<TLBEntry> &vec, size_t idx, levelEntry lvl) {
        // Null check first
        if (!&vec || vec.capacity() == 0) {
            // This should never happen - debug break
            std::cerr << "FATAL: Null vector in ensure_level!" << std::endl;
            std::abort();
        }

        if (idx >= vec.size()) {
            vec.resize(idx + 1, TLBEntry{});
        }
        TLBEntry &e = vec[idx];

        // Clean up old payload
        if (e.is_table && e.payload.table) {
            delete e.payload.table;
        }

        e.level    = lvl;
        e.is_table = (lvl != DATA);

        if (lvl == DATA) {
            e.payload.data = TLBEntryData{};
        } else {
            e.payload.table = new TLBTable();
        }
        return e;
    }


    void LazyHybridTLB::dump_tree(uint64_t vpn_) const {
        std::cout << "\n=== VPN BREAKDOWN 0x" << std::hex << vpn_ << std::dec << " ===" << std::endl;
        std::cout << "FULL:     0x" << std::hex << vpn_ << std::dec << std::endl;
        std::cout << "PT2:      " << GET_PT2(vpn_) << " (24 bits, 0x" << std::hex << GET_PT2(vpn_) << std::dec << ")" << std::endl;
        std::cout << "PT1:      " << GET_PT1(vpn_) << " (16 bits, 0x" << std::hex << GET_PT1(vpn_) << std::dec << ")" << std::endl;
        std::cout << "PT:       " << GET_PT(vpn_)  << " (12 bits, 0x" << std::hex << GET_PT(vpn_)  << std::dec << ")" << std::endl;
        std::cout << "OFFSET:   " << GET_OFFSET(vpn_) << " (12 bits, 0x" << std::hex << GET_OFFSET(vpn_) << std::dec << ")" << std::endl;

        uint32_t pt2 = GET_PT2(vpn_);
        uint16_t pt1 = GET_PT1(vpn_);
        uint16_t pt  = GET_PT(vpn_);

        std::cout << "\nTLB PATH: root[" << pt2 << "] -> pt1[" << pt1 << "] -> pt[" << pt << "] -> data" << std::endl;

        if (pt2 < root.size()) {
            const TLBNode& n2 = root[pt2];
            std::cout << "PT2[" << pt2 << "]: " << (int)n2.type << std::endl;
            if (pt1 < n2.children.size() && n2.children[pt1]) {
                const TLBNode& n1 = *n2.children[pt1];
                std::cout << "PT1[" << pt1 << "]: " << (int)n1.type << std::endl;
                if (pt < n1.children.size() && n1.children[pt]) {
                    const TLBNode& data_node = *n1.children[pt];
                    std::cout << "PT[" << pt << "]: " << (int)data_node.type << std::endl;
                    std::cout << "  DATA: " << data_node.data.to_string() << std::endl;  // data_node.data
                }
            }
        }

    }


    void LazyHybridTLB::dump_stats() const {
        std::cout << "\n=== LAZYHYBRIDTLB STATS ===" << std::endl;
        std::cout << "Root entries: " << root.size() << std::endl;

        int pt2_active = 0, pt1_active = 0, data_entries = 0;
        for (const auto& n2 : root) {
            if (n2.type == PT2) {
                pt2_active++;
                for (auto* n1 : n2.children) {
                    if (n1 && n1->type == PT1) {
                        pt1_active++;
                        data_entries += n1->children.size();
                    }
                }
            }
        }
        std::cout << "PT2 active: " << pt2_active
                  << ", PT1 active: " << pt1_active
                  << ", DATA slots: " << data_entries << std::endl;
    }




    LazyHybridTLB::LazyHybridTLB() {
        root.resize(1); // Pre-allocate root level
    }


    LazyHybridTLB::~LazyHybridTLB() = default;  // Vector destructor maneja todo


    void LazyHybridTLB::translate(uint64_t ptr_, vm::type_ptr_mapped type, vm::ptr_mapped ptr_mapped) {
        uint32_t pt2 = GET_PT2(ptr_);
        uint16_t pt1 = GET_PT1(ptr_);
        uint16_t pt  = GET_PT(ptr_);

        if (pt2 >= root.size()) {
            root.resize(pt2 + 1);
        }


        // NIVEL 1: PT2 (root)
        if (pt2 >= root.size()) root.resize(pt2 + 1);
        TLBNode& pt2_node = root[pt2];
        pt2_node.type = PT2;

        // NIVEL 2: PT1 (children de PT2)
        if (pt1 >= pt2_node.children.size()) pt2_node.children.resize(pt1 + 1, nullptr);
        if (!pt2_node.children[pt1]) pt2_node.children[pt1] = new TLBNode();
        TLBNode& pt1_node = *pt2_node.children[pt1];
        pt1_node.type = PT1;

        // NIVEL 3: PT (children de PT1) - NUEVO
        if (pt >= pt1_node.children.size()) pt1_node.children.resize(pt + 1, nullptr);
        if (!pt1_node.children[pt]) pt1_node.children[pt] = new TLBNode();
        TLBNode& pt_node = *pt1_node.children[pt];
        pt_node.type = DATA;

        pt_node.data = TLBEntryData{type, ptr_mapped};
    }

    void LazyHybridTLB::clear_tlb_entry(uint64_t page_vaddr) {
        // Resetear a estado inválido (simplificado)
        this->translate(page_vaddr, vm::MAPPED_PTR_VM,
                         vm::ptr_mapped{.ptr_vm = vm::vm_map_ptr{0}});
    }


    TLBEntryData* LazyHybridTLB::get_entry(uint64_t ptr_) const {
        uint32_t pt2 = GET_PT2(ptr_);
        if (pt2 >= root.size()) return nullptr;

        const TLBNode& pt2_node = root[pt2];
        if (pt2_node.type != PT2) return nullptr;

        uint16_t pt1 = GET_PT1(ptr_);
        if (pt1 >= pt2_node.children.size() || !pt2_node.children[pt1]) return nullptr;

        const TLBNode& pt1_node = *pt2_node.children[pt1];
        if (pt1_node.type != PT1) return nullptr;

        uint16_t pt = GET_PT(ptr_);
        if (pt >= pt1_node.children.size() || !pt1_node.children[pt]) return nullptr;

        const TLBNode& pt_node = *pt1_node.children[pt];
        if (pt_node.type != DATA) return nullptr;

        return const_cast<TLBEntryData*>(&pt_node.data);
    }

    void* LazyHybridTLB::get_real_host_ptr_of_vptr(uint64_t vptr_) const {
        const TLBEntryData* entry = get_entry(vptr_);
        if (entry == nullptr) return nullptr;

        // si el puntero obtenido no es un puntero mapeado de host, entonces no se devuelve
        if (vm::MAPPED_PTR_HOST != entry->type_address) return nullptr;

        return entry->address.ptr_host;
    }

} // namespace tlb
