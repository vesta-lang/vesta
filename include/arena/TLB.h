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

#ifndef TLB_H
#define TLB_H

#include <cstdint>
#include <memory>
#include <vector>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <variant>
#include <sstream>
#include <iomanip>
#include <memory_resource>

#include "arena/arena.h"
#include "ftxui/component/component_base.hpp"

#define GET_OFFSET(address)  (address & 0xFFF)
#define GET_PT(address)      ((address >> 12) & 0xFFF)
#define GET_PT1(address)     ((address >> 24) & 0xFFFF)
#define GET_PT2(address)     ((address >> 40) & 0xFFFFFF)

namespace tlb {
    struct TLBEntry;
    class LazyHybridTLB;
    struct PageNode;

    inline std::ostream& operator<<(std::ostream& os, const vm::vm_map_ptr& ptr) {
        os << "0x" << std::hex << ptr.raw << std::dec;
        return os;
    }

    /**
     * El TLB permite realizar traducciones entre direcciones virtuales
     * y direcciones reales, sin tener que crear tablar enormes para realizar
     * la traducion.
     *
     * **Offset**    = 0xFFF     (`12bits`) [`12bits`] offset
     * Page Table    = 0xFFF     (`12bits`) [`24bits`] PT
     * Page Table1   = 0xFFFF    (`16bits`) [`40bits`] PT1
     * Page Table2   = 0xFFFFFF  (`24bits`) [`64bits`] PT2
     *
     * Cada entrada entrada en la PageTable (PT) contiene una estructura TLB_entry
     */
    typedef struct TLBEntryData {
        /**
         * Tipo de direccion mapeada por esta entrada.
         */
        vm::type_ptr_mapped type_address;

        /**
         * direccion real que se mapea. En una instancia o en un manager
         * a nivel remoto o local.
         */
        vm::ptr_mapped address;

        [[nodiscard]] std::string to_string() const {
            std::ostringstream oss;

            // TIPO HUMANO
            switch(type_address) {
                case vm::MAPPED_PTR_HOST:  oss << "HOST";    break;
                case vm::MAPPED_PTR_VM: oss << "GUEST";   break;
                case vm::MAPPED_PTR_REMOTE:oss << "REMOTE";  break;
                default: oss << "NONE(" << (int)type_address << ")";
            }

            oss << "{";

            // DIRECCIONES según tipo
            switch(type_address) {
                case vm::MAPPED_PTR_HOST:
                    oss << "host=" << std::hex << address.ptr_host << std::dec;
                    break;
                case vm::MAPPED_PTR_VM:
                    oss << "guest=" << std::hex << address.ptr_vm << std::dec;
                    break;
                case vm::MAPPED_PTR_REMOTE:
                    oss << "remote{id=" << std::hex << address.ptr_remote
                        << ", offset=" << address.ptr_remote << std::dec << "}";
                    break;
                default:
                    oss << "addr=" << std::hex << address.ptr_host << std::dec;
            }

            oss << "}";
            return oss.str();
        }

    } TLBEntryData;


    typedef enum levelEntry { DATA, PT, PT1, PT2 } level;
    typedef struct TLBTable {
        std::vector<TLBEntry> entry;
        TLBTable() : entry(1) {}
    } TLBTable;

    typedef struct TLBEntry {
        levelEntry level;
        union {
            TLBEntryData data;
            TLBTable* table;
        } payload;
        bool is_table;  // true = table, false = data

        TLBEntry() : level(DATA), is_table(false) {}
        ~TLBEntry() {
            if (is_table && payload.table) {
                delete payload.table;
            }
        }
    } TLBEntry;

    typedef struct TLBNode {
        levelEntry type = DATA;
        TLBEntryData data;
        std::vector<TLBNode*> children;  // nullptr = leaf (DATA), else table

        TLBNode() : children(1, nullptr) {}
        ~TLBNode() { for (auto* child : children) delete child; }
    } TLBNode;

    /**
     * Los page table PT, PT1, y PT2 usan todos la misma estructura.
     * PT2 (TLB_page) -> PT1 (TLB_page) -> PT -> TLB_Entry
     */
    class LazyHybridTLB {
        std::vector<TLBNode> root{1};

    public:
        ~LazyHybridTLB();

        void translate(uint64_t ptr, vm::type_ptr_mapped type, vm::ptr_mapped ptr_mapped);

        void clear_tlb_entry(uint64_t page_vaddr);

        [[nodiscard]] TLBEntryData*get_entry(uint64_t ptr) const;

        void *get_real_host_ptr_of_vptr(uint64_t vptr_) const;

        void dump_tree(uint64_t vpn_) const;

        void dump_stats() const;

        LazyHybridTLB();
    };
}

#endif //TLB_H
