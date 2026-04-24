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
 * @file gc_heap.cpp
 * @brief Implementacion del heap generacional con GC tri-color de VestaVM.
 *
 * Implementa @c gc::GCHeap: asignacion en nursery, promocion a old-gen,
 * ciclos de recoleccion mayor/menor, write-barrier y las fases del algoritmo
 * tri-color mark-and-sweep (blanco/gris/negro).
 */
#include "gc/gc_heap.h"

#include <cstring>
#include <cassert>

namespace gc {

    // -------------------------------------------------------------------------
    // Constructor / destructor
    // -------------------------------------------------------------------------

    GcHeap::GcHeap(vm::ArenaManager &arena_mgr, size_t nursery_bytes, size_t old_threshold)
        : arena_mgr_(arena_mgr), nursery_size_(nursery_bytes), old_threshold_(old_threshold)
    {
        nursery_arena_id_ = arena_mgr_.create_arena(nursery_bytes,
                                                     vm::MemPerm::READ | vm::MemPerm::WRITE);
        const vm::Arena *a = arena_mgr_.get_arena(nursery_arena_id_);
        assert(a && a->ptr && "GcHeap: no se pudo crear la Nursery");

        nursery_base_ = static_cast<uint8_t *>(a->ptr);
        nursery_bump_ = nursery_base_;
        nursery_end_  = nursery_base_ + nursery_bytes;
    }

    GcHeap::~GcHeap() {
        for (auto &b : old_blocks_)
            arena_mgr_.free_arena(b.arena_id);
        old_blocks_.clear();

        arena_mgr_.free_arena(nursery_arena_id_);
        nursery_base_ = nursery_bump_ = nursery_end_ = nullptr;
    }

    // -------------------------------------------------------------------------
    // Gestion de handles
    // -------------------------------------------------------------------------

    GcHandle GcHeap::new_handle(uint8_t *addr) {
        if (!free_handles_.empty()) {
            GcHandle h = free_handles_.back();
            free_handles_.pop_back();
            handles_[h] = { addr, true };
            return h;
        }
        GcHandle h = static_cast<GcHandle>(handles_.size());
        handles_.push_back({ addr, true });
        return h;
    }

    void GcHeap::release_handle(GcHandle h) {
        if (h >= handles_.size()) return;
        handles_[h] = { nullptr, false };
        free_handles_.push_back(h);
    }

    // -------------------------------------------------------------------------
    // alloc
    // -------------------------------------------------------------------------

    GcHandle GcHeap::alloc(size_t size) {
        constexpr size_t ALIGN = 8;
        size_t total = (sizeof(GcHeader) + size + ALIGN - 1) & ~(ALIGN - 1);

        auto init_obj = [&](uint8_t *raw, GcGen gen) -> GcHandle {
            auto *hdr  = reinterpret_cast<GcHeader *>(raw);
            hdr->size  = static_cast<uint32_t>(size);
            hdr->color = GcColor::WHITE;
            hdr->gen   = gen;
            std::memset(raw + sizeof(GcHeader), 0, size);

            stats_.alloc_count++;
            stats_.alloc_bytes += size;
            size_t nu = nursery_used();
            if (nu > stats_.peak_nursery) stats_.peak_nursery = nu;
            if (old_used_ > stats_.peak_old) stats_.peak_old = old_used_;

            return new_handle(raw);
        };

        // Fast-path: espacio en Nursery
        if (nursery_bump_ + total <= nursery_end_) {
            uint8_t *raw   = nursery_bump_;
            nursery_bump_ += total;
            return init_obj(raw, GcGen::YOUNG);
        }

        // Nursery llena: minor GC y reintentar
        minor_gc();

        if (nursery_bump_ + total <= nursery_end_) {
            uint8_t *raw   = nursery_bump_;
            nursery_bump_ += total;
            return init_obj(raw, GcGen::YOUNG);
        }

        // Sigue sin espacio: asignar directo en OldGen
        uint8_t *raw = alloc_in_old(total);
        if (!raw) return GC_NULL_HANDLE;
        return init_obj(raw, GcGen::OLD);
    }

    // -------------------------------------------------------------------------
    // deref
    // -------------------------------------------------------------------------

    uint8_t *GcHeap::deref(GcHandle h) {
        if (h >= handles_.size() || !handles_[h].live) return nullptr;
        return handles_[h].addr + sizeof(GcHeader);
    }

    // -------------------------------------------------------------------------
    // write_barrier  - O(1) con unordered_set
    // -------------------------------------------------------------------------

    void GcHeap::write_barrier(GcHandle old_handle) {
        remembered_set_.insert(old_handle);
    }

    // -------------------------------------------------------------------------
    // do_evacuate - nucleo de evacuacion sin comprobacion de liveness
    // -------------------------------------------------------------------------

    void GcHeap::do_evacuate(GcHandle h) {
        uint8_t *src_raw = handles_[h].addr;
        if (!src_raw) return;

        auto *hdr = reinterpret_cast<GcHeader *>(src_raw);
        if (hdr->color == GcColor::BLACK || hdr->gen == GcGen::OLD) return;

        size_t   total   = (sizeof(GcHeader) + hdr->size + 7) & ~7ULL;
        uint8_t *dst_raw = alloc_in_old(total);
        if (!dst_raw) return;

        std::memcpy(dst_raw, src_raw, total);

        auto *dst_hdr  = reinterpret_cast<GcHeader *>(dst_raw);
        dst_hdr->gen   = GcGen::OLD;
        dst_hdr->color = GcColor::BLACK;

        handles_[h].addr = dst_raw;

        // Forward pointer en payload original para detectar doble evacuacion
        uint8_t *src_payload = src_raw + sizeof(GcHeader);
        std::memcpy(src_payload, &dst_raw, sizeof(void *));
        hdr->color = GcColor::BLACK;

        stats_.promoted_count++;
        stats_.promoted_bytes += hdr->size;
        if (old_used_ > stats_.peak_old) stats_.peak_old = old_used_;
    }

    void GcHeap::evacuate_object(GcHandle h) {
        if (h >= handles_.size() || !handles_[h].addr) return;
        do_evacuate(h);
    }

    // -------------------------------------------------------------------------
    // scan_young_refs - escanea payload buscando handles VIVOS que apunten a YOUNG
    // -------------------------------------------------------------------------

    void GcHeap::scan_young_refs(GcHandle h, std::vector<GcHandle>& worklist) {
        if (!handles_[h].addr) return;

        auto    *hdr     = reinterpret_cast<GcHeader *>(handles_[h].addr);
        uint8_t *payload = handles_[h].addr + sizeof(GcHeader);
        size_t   sz      = hdr->size;

        for (size_t off = 0; off + sizeof(GcHandle) <= sz; off += sizeof(GcHandle)) {
            GcHandle ref;
            std::memcpy(&ref, payload + off, sizeof(GcHandle));

            if (ref == GC_NULL_HANDLE || ref >= static_cast<GcHandle>(handles_.size())) continue;
            // Solo seguir handles vivos para evitar falsos positivos con datos numericos
            if (!handles_[ref].live || !handles_[ref].addr) continue;

            uint8_t *ref_raw = handles_[ref].addr;
            if (ref_raw < nursery_base_ || ref_raw >= nursery_end_) continue;

            auto *ref_hdr = reinterpret_cast<GcHeader *>(ref_raw);
            if (ref_hdr->color == GcColor::BLACK) continue;

            do_evacuate(ref);
            worklist.push_back(ref);
        }
    }

    // -------------------------------------------------------------------------
    // mark_reachable - BFS sobre handles VIVOS embebidos en payloads OLD
    // -------------------------------------------------------------------------

    void GcHeap::mark_reachable(GcHandle h, std::vector<GcHandle>& worklist) {
        if (!handles_[h].addr) return;

        auto    *hdr     = reinterpret_cast<GcHeader *>(handles_[h].addr);
        uint8_t *payload = handles_[h].addr + sizeof(GcHeader);
        size_t   sz      = hdr->size;

        for (size_t off = 0; off + sizeof(GcHandle) <= sz; off += sizeof(GcHandle)) {
            GcHandle ref;
            std::memcpy(&ref, payload + off, sizeof(GcHandle));

            if (ref == GC_NULL_HANDLE || ref >= static_cast<GcHandle>(handles_.size())) continue;
            // Solo seguir handles vivos: un handle soltado con drop() no mantiene
            // vivo al objeto aunque su valor este embebido en el payload
            if (!handles_[ref].live || !handles_[ref].addr) continue;

            auto *ref_hdr = reinterpret_cast<GcHeader *>(handles_[ref].addr);
            if (ref_hdr->gen != GcGen::OLD || ref_hdr->color != GcColor::WHITE) continue;

            ref_hdr->color = GcColor::BLACK;
            worklist.push_back(ref);
        }
    }

    // -------------------------------------------------------------------------
    // Minor GC - Cheney-style
    // -------------------------------------------------------------------------

    void GcHeap::minor_gc() {
        stats_.minor_gc_count++;

        // Evacuar todos los objetos YOUNG con handle vivo
        for (GcHandle h = 0; h < static_cast<GcHandle>(handles_.size()); ++h) {
            if (!handles_[h].live || !handles_[h].addr) continue;
            uint8_t *raw = handles_[h].addr;
            if (raw < nursery_base_ || raw >= nursery_end_) continue;
            auto *hdr = reinterpret_cast<GcHeader *>(raw);
            if (hdr->gen == GcGen::YOUNG)
                do_evacuate(h);
        }

        // Raices adicionales del remembered_set: OLD objects con refs a YOUNG vivos
        // Cubre el caso: objeto OLD escribe ref YOUNG via GCWB y el objeto YOUNG
        // solo es alcanzable desde ese campo (sin handle propio en el bytecode)
        std::vector<GcHandle> rs_worklist;
        for (GcHandle old_h : remembered_set_)
            scan_young_refs(old_h, rs_worklist);

        remembered_set_.clear();
        nursery_bump_ = nursery_base_;

        if (old_used_ >= old_threshold_)
            major_gc();
    }

    // -------------------------------------------------------------------------
    // Major GC - mark-and-sweep tri-color transitivo sobre OldGen
    // -------------------------------------------------------------------------

    void GcHeap::major_gc() {
        stats_.major_gc_count++;

        // PRE-MARK: todos los objetos OldGen vivos (no DEAD) -> WHITE
        for (auto &block : old_blocks_) {
            uint8_t *cursor = block.ptr;
            uint8_t *end    = block.ptr + block.size;
            while (cursor + sizeof(GcHeader) <= end) {
                auto *hdr = reinterpret_cast<GcHeader *>(cursor);
                if (hdr->size == 0) break;
                if (hdr->color != GcColor::DEAD)
                    hdr->color = GcColor::WHITE;
                cursor += (sizeof(GcHeader) + hdr->size + 7) & ~7ULL;
            }
        }

        // MARK: BFS desde handles vivos con objeto OLD
        std::vector<GcHandle> worklist;
        for (GcHandle h = 0; h < static_cast<GcHandle>(handles_.size()); ++h) {
            if (!handles_[h].live || !handles_[h].addr) continue;
            auto *hdr = reinterpret_cast<GcHeader *>(handles_[h].addr);
            if (hdr->gen == GcGen::OLD && hdr->color == GcColor::WHITE) {
                hdr->color = GcColor::BLACK;
                worklist.push_back(h);
            }
        }

        // BFS transitivo: seguir handles VIVOS embebidos en payloads
        for (size_t i = 0; i < worklist.size(); ++i)
            mark_reachable(worklist[i], worklist);

        // SWEEP: WHITE (sin raiz) -> DEAD
        for (auto &block : old_blocks_) {
            uint8_t *cursor = block.ptr;
            uint8_t *end    = block.ptr + block.size;

            while (cursor + sizeof(GcHeader) <= end) {
                auto  *hdr   = reinterpret_cast<GcHeader *>(cursor);
                if (hdr->size == 0) break;

                size_t total = (sizeof(GcHeader) + hdr->size + 7) & ~7ULL;

                if (hdr->color == GcColor::WHITE) {
                    stats_.freed_count++;
                    stats_.freed_bytes += hdr->size;
                    old_used_ -= total;
                    hdr->color = GcColor::DEAD;
                }

                cursor += total;
            }
        }
    }

    // -------------------------------------------------------------------------
    // alloc_in_old
    // -------------------------------------------------------------------------

    uint8_t *GcHeap::alloc_in_old(size_t total_bytes) {
        for (auto &block : old_blocks_) {
            uint8_t *cursor = block.ptr;
            uint8_t *end    = block.ptr + block.size;

            while (cursor + sizeof(GcHeader) <= end) {
                auto *hdr = reinterpret_cast<GcHeader *>(cursor);

                if (hdr->size == 0) {
                    size_t available = static_cast<size_t>(end - cursor);
                    if (available >= total_bytes) {
                        old_used_ += total_bytes;
                        return cursor;
                    }
                    break;
                }

                if (hdr->color == GcColor::DEAD) {
                    size_t slot_total = (sizeof(GcHeader) + hdr->size + 7) & ~7ULL;
                    if (slot_total >= total_bytes) {
                        old_used_ += total_bytes;
                        return cursor;
                    }
                }

                cursor += (sizeof(GcHeader) + hdr->size + 7) & ~7ULL;
            }
        }

        size_t   block_size = total_bytes < 64 * 1024 ? 64 * 1024 : total_bytes * 2;
        uint64_t aid        = arena_mgr_.create_arena(block_size, vm::MemPerm::READ | vm::MemPerm::WRITE);
        const vm::Arena *a  = arena_mgr_.get_arena(aid);
        if (!a || !a->ptr) return nullptr;

        old_blocks_.push_back({ static_cast<uint8_t *>(a->ptr), block_size, aid });
        old_used_ += total_bytes;
        return static_cast<uint8_t *>(a->ptr);
    }

} // namespace gc
