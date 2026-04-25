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
#include "loader/oop_types.h"

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

        // WEAK SWEEP: anular referencias debiles a objetos recolectados
        // Si el handle apuntado por una WeakEntry esta muerto, poner target = GC_NULL_HANDLE
        for (auto &entry : weak_table_) {
            if (!entry.live) continue; // entrada ya liberada
            if (entry.target == GC_NULL_HANDLE) continue; // ya anulada

            // verificar si el objeto sigue vivo (color != DEAD) en la tabla de handles
            bool alive = false;
            if (entry.target < handles_.size()) {
                const auto &he = handles_[entry.target];
                if (he.addr != nullptr && he.live) {
                    // he.addr apunta al GcHeader del objeto; leer su color
                    const auto *ghdr = reinterpret_cast<const GcHeader *>(he.addr);
                    alive = (ghdr->color != GcColor::DEAD);
                }
            }
            if (!alive) entry.target = GC_NULL_HANDLE; // objeto muerto: anular referencia
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

    // =========================================================================
    //  Tabla de referencias debiles
    // =========================================================================

    /**
     * @brief Registra un GcHandle en la tabla de referencias debiles.
     *
     * Busca primero un slot libre (live==false) para reutilizarlo.  Si no hay
     * ninguno disponible, inserta una nueva entrada al final.
     *
     * @param target GcHandle del objeto a observar debilmente.
     * @return Indice opaco uint32_t de la entrada en la tabla.
     */
    uint32_t GcHeap::alloc_weak(GcHandle target) {
        // reutilizar slots liberados antes de crecer la tabla
        for (uint32_t i = 0; i < static_cast<uint32_t>(weak_table_.size()); ++i) {
            if (!weak_table_[i].live) {
                weak_table_[i].target = target;
                weak_table_[i].live   = true;
                return i;
            }
        }
        // ningun slot libre: anadir al final de la tabla
        uint32_t idx = static_cast<uint32_t>(weak_table_.size());
        weak_table_.push_back({ target, true });
        return idx;
    }

    /**
     * @brief Resuelve un indice de weak ref al GcHandle subyacente.
     *
     * Si el objeto fue recolectado durante el ultimo major GC el campo target
     * habra sido puesto a GC_NULL_HANDLE por el barrido de weak refs.
     *
     * @param idx Indice opaco devuelto por alloc_weak().
     * @return GcHandle del objeto si sigue vivo; GC_NULL_HANDLE si fue recolectado
     *         o si el indice esta fuera de rango.
     */
    GcHandle GcHeap::deref_weak(uint32_t idx) const {
        if (idx >= static_cast<uint32_t>(weak_table_.size())) return GC_NULL_HANDLE;
        const auto &entry = weak_table_[idx];
        if (!entry.live) return GC_NULL_HANDLE; // slot liberado: referencia invalida
        return entry.target; // GC_NULL_HANDLE si el objeto fue recolectado, handle valido si no
    }

    /**
     * @brief Libera una entrada de la tabla de referencias debiles.
     *
     * Marca el slot como libre (live=false) para que pueda ser reutilizado.
     * Si idx esta fuera de rango o el slot ya estaba libre, no hace nada.
     *
     * @param idx Indice opaco devuelto por alloc_weak().
     */
    void GcHeap::free_weak(uint32_t idx) {
        if (idx >= static_cast<uint32_t>(weak_table_.size())) return;
        weak_table_[idx].live   = false;         // marcar como slot libre
        weak_table_[idx].target = GC_NULL_HANDLE; // limpiar el handle por seguridad
    }

    // -------------------------------------------------------------------------
    // Monitor / sincronizacion
    // -------------------------------------------------------------------------

    /**
     * @brief Intenta adquirir el monitor del objeto referenciado por @p h.
     *
     * Si owner_pid == 0 (libre): asigna el monitor a @p local_pid,
     * establece lock_depth = 1 y devuelve true.
     * Si owner_pid == local_pid (lock reentrante): incrementa lock_depth y
     * devuelve true.
     * Si lo posee otro proceso: devuelve false (el llamante debe bloquear).
     *
     * @param h         Handle del objeto cuyo monitor se quiere adquirir.
     * @param local_pid PID local del proceso solicitante.
     * @return true si el monitor fue adquirido o incrementado.
     */
    bool GcHeap::monitor_try_acquire(GcHandle h, uint32_t local_pid) {
        uint8_t *ptr = deref(h);
        if (ptr == nullptr) return false; // handle invalido: no se puede adquirir

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(ptr);

        if (hdr->owner_pid == 0) {
            hdr->owner_pid  = local_pid; // adquirir el monitor
            hdr->lock_depth = 1;
            return true;
        }
        if (hdr->owner_pid == local_pid) {
            hdr->lock_depth++; // lock reentrante: incrementar profundidad
            return true;
        }
        return false; // monitor ocupado por otro proceso
    }

    /**
     * @brief Libera el monitor del objeto referenciado por @p h.
     *
     * Decrementa lock_depth.  Cuando llega a 0 el monitor queda libre
     * (owner_pid = 0).  Extrae el primer proceso de la cola de espera
     * para despertarlo.
     *
     * @param h         Handle del objeto cuyo monitor se libera.
     * @param local_pid PID local del propietario actual (para validar).
     * @return PID codificado del siguiente proceso en cola (0 si vacia).
     */
    uint64_t GcHeap::monitor_release(GcHandle h, uint32_t local_pid) {
        uint8_t *ptr = deref(h);
        if (ptr == nullptr) return 0;

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(ptr);
        if (hdr->owner_pid != local_pid) return 0; // no es el propietario

        if (hdr->lock_depth > 1) {
            hdr->lock_depth--; // reducir nivel de reentrada sin liberar el monitor
            return 0;
        }

        // lock_depth llega a 0: liberar el monitor
        hdr->owner_pid  = 0;
        hdr->lock_depth = 0;

        return monitor_pop_waiter(h); // devolver el siguiente proceso a despertar
    }

    /**
     * @brief Anade un PID codificado a la cola de espera del monitor de @p h.
     *
     * @param h           Handle del objeto cuyo monitor tiene la cola.
     * @param encoded_pid PID codificado: (scheduler_id << 32) | local_pid.
     */
    void GcHeap::monitor_add_waiter(GcHandle h, uint64_t encoded_pid) {
        monitor_waiters_[h].push_back(encoded_pid); // insertar al final de la cola FIFO
    }

    /**
     * @brief Extrae y devuelve el primer PID de la cola de espera del monitor de @p h.
     *
     * @param h Handle del objeto cuya cola de espera se consulta.
     * @return PID codificado del primer proceso en cola, o 0 si la cola esta vacia.
     */
    uint64_t GcHeap::monitor_pop_waiter(GcHandle h) {
        auto it = monitor_waiters_.find(h);
        if (it == monitor_waiters_.end() || it->second.empty()) return 0;

        uint64_t pid = it->second.front(); // extraer el primero de la cola FIFO
        it->second.erase(it->second.begin());
        if (it->second.empty()) monitor_waiters_.erase(it); // limpiar entrada vacia
        return pid;
    }

    /**
     * @brief Extrae y devuelve todos los PID de la cola de espera del monitor de @p h.
     *
     * La cola queda vacia tras la llamada.
     *
     * @param h Handle del objeto cuya cola de espera se vacia.
     * @return Vector con todos los PID codificados.
     */
    std::vector<uint64_t> GcHeap::monitor_pop_all_waiters(GcHandle h) {
        auto it = monitor_waiters_.find(h);
        if (it == monitor_waiters_.end()) return {}; // cola inexistente: devolver vacio

        std::vector<uint64_t> result = std::move(it->second); // mover para evitar copia
        monitor_waiters_.erase(it); // limpiar la entrada
        return result;
    }

} // namespace gc
