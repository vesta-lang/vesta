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

            // stats: incrementos simples, sin ramas extra
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
    // write_barrier
    // -------------------------------------------------------------------------

    void GcHeap::write_barrier(GcHandle old_handle) {
        for (GcHandle h : remembered_set_) {
            if (h == old_handle) return;
        }
        remembered_set_.push_back(old_handle);
    }

    // -------------------------------------------------------------------------
    // Minor GC - Cheney-style copy de Nursery a OldGen
    // -------------------------------------------------------------------------

    void GcHeap::evacuate_object(GcHandle h) {
        if (h >= handles_.size() || !handles_[h].live) return;

        uint8_t *src_raw = handles_[h].addr;
        auto    *hdr     = reinterpret_cast<GcHeader *>(src_raw);

        if (hdr->color == GcColor::BLACK || hdr->gen == GcGen::OLD) return;

        size_t total     = (sizeof(GcHeader) + hdr->size + 7) & ~7ULL;
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

        // stats: promocion
        stats_.promoted_count++;
        stats_.promoted_bytes += hdr->size;
        if (old_used_ > stats_.peak_old) stats_.peak_old = old_used_;
    }

    void GcHeap::minor_gc() {
        stats_.minor_gc_count++;

        for (GcHandle h = 0; h < static_cast<GcHandle>(handles_.size()); ++h) {
            if (!handles_[h].live) continue;
            uint8_t *raw = handles_[h].addr;
            if (raw < nursery_base_ || raw >= nursery_end_) continue;
            auto *hdr = reinterpret_cast<GcHeader *>(raw);
            if (hdr->gen == GcGen::YOUNG)
                evacuate_object(h);
        }

        remembered_set_.clear();
        nursery_bump_ = nursery_base_; // reset bump: toda la Nursery libre

        if (old_used_ >= old_threshold_)
            major_gc();
    }

    // -------------------------------------------------------------------------
    // Major GC - mark-and-sweep sobre OldGen
    // -------------------------------------------------------------------------

    void GcHeap::major_gc() {
        stats_.major_gc_count++;

        // PRE-MARK: todos los objetos OldGen vivos (no DEAD) -> WHITE.
        // Necesario porque los objetos llegan a OldGen con BLACK tras la evacuacion,
        // y los handles soltados con drop() no tienen ningun mecanismo para reset.
        // Sin este paso, un objeto evacuado con BLACK y luego soltado nunca seria
        // barrido por el SWEEP (seguiria siendo BLACK aunque ya no tenga raiz).
        for (auto &block : old_blocks_) {
            uint8_t *cursor = block.ptr;
            uint8_t *end    = block.ptr + block.size;
            while (cursor + sizeof(GcHeader) <= end) {
                auto *hdr = reinterpret_cast<GcHeader *>(cursor);
                if (hdr->size == 0) break;
                if (hdr->color != GcColor::DEAD)  // respetar slots ya liberados
                    hdr->color = GcColor::WHITE;
                cursor += (sizeof(GcHeader) + hdr->size + 7) & ~7ULL;
            }
        }

        // MARK: handles vivos en OldGen -> BLACK
        for (auto &entry : handles_) {
            if (!entry.live || !entry.addr) continue;
            auto *hdr = reinterpret_cast<GcHeader *>(entry.addr);
            if (hdr->gen == GcGen::OLD)
                hdr->color = GcColor::BLACK;
        }

        // SWEEP: WHITE (sin raiz) -> DEAD; el slot fisico queda reutilizable.
        // No se busca el handle porque drop() ya lo libero; solo se reclama
        // el espacio contabilizado en old_used_.
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
                    hdr->color = GcColor::DEAD;  // slot reutilizable; size se preserva
                }
                // BLACK: alcanzable, no tocar
                // DEAD:  ya liberado, no tocar (double-free protection)

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
                    // Fin de la region usada del bloque: espacio libre contiguo
                    size_t available = static_cast<size_t>(end - cursor);
                    if (available >= total_bytes) {
                        old_used_ += total_bytes;
                        return cursor;
                    }
                    break;
                }

                if (hdr->color == GcColor::DEAD) {
                    // Slot liberado por sweep: reutilizar si el tamaño es suficiente.
                    // El campo size se preservo al marcar DEAD para poder calcular esto.
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
