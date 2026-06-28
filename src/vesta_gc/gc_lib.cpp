/**
 * @file vesta_gc/gc_lib.cpp
 * @brief Implementacion de la C-ABI de `libvesta_gc` (GC opt-in de Vex en AOT).
 *
 * Envuelve un `gc::GcHeap` GLOBAL (singleton) sin `ProcessVM`: el mismo motor
 * generacional mark-sweep del interprete/JIT, pero con un unico heap de proceso.
 * Las raices se descubriran via stackmaps precisos sobre frames nativos
 * (Inc 2); en este Inc 0, `owner_proc_` es nullptr -> `major_gc` conserva todos
 * los handles vivos (no colecta todavia), suficiente para validar alloc/deref.
 *
 * NOTA de enlazado (Inc 0b pendiente): `gc_heap.cpp` aun arrastra
 * `proceso_runtime.h` por los caminos guardados por `owner_proc_ != nullptr`
 * (shared-heap, scan conservativo).  Para un `.o` freestanding linkable sin la
 * VM hay que desacoplar esos sitios via una interfaz RootProvider (Phase C.3).
 * Hasta entonces esta TU se compila dentro del proyecto (linka la VM) y valida
 * el motor del GC operando ProcessVM-less.
 */

#include "vesta_gc/gc_lib.h"

#include "arena/arena_manager.h"
#include "gc/gc_heap.h"

namespace {

/**
 * @brief ArenaManager global del GC de AOT (reserva mmap/VirtualAlloc).
 * @return Referencia al manager unico, construido en el primer uso.
 */
vm::ArenaManager &gc_arena() {
    static vm::ArenaManager arena;
    return arena;
}

/**
 * @brief Heap global del GC de AOT.  Mismos parametros que el del ProcessVM
 *        (nursery 2 MiB, umbral old 8 MiB).  `owner_proc_` queda nullptr.
 * @return Referencia al GcHeap unico.
 * @note El static local garantiza orden de construccion: primero @c gc_arena()
 *       (lo invoca el inicializador), luego el GcHeap.
 */
gc::GcHeap &gc_heap() {
    static gc::GcHeap heap(gc_arena(), 2u * 1024u * 1024u, 8u * 1024u * 1024u);
    // Modo AOT: raices SOLO por stackmaps precisos (frames nativos via
    // JitRegistry) + external_refs + pending.  Sin esto el major_gc conservaria
    // todo (no colectaria).  v1 no-moving: vex_gc_alloc usa alloc_pinned ->
    // OldGen, el nursery queda vacio (sin riesgo de interior-ptr stale al
    // mover).  Set idempotente en cada acceso (1 store, a prueba de orden de
    // init de statics).
    heap.set_aot_mode(true);
    return heap;
}

} // namespace

extern "C" {

void vex_gc_init(void) {
    // Forzar la construccion del heap (idempotente: el static local solo se
    // inicializa una vez).
    (void)gc_heap();
}

uint32_t vex_gc_alloc(uint64_t size) {
    // v1 no-moving: pinned -> OldGen (el nursery queda vacio).  El GC colecta
    // por mark-sweep con raices precisas; sin compactacion (optimizacion v2).
    return static_cast<uint32_t>(
        gc_heap().alloc_pinned(static_cast<size_t>(size)));
}

uint8_t *vex_gc_alloc_ptr(uint64_t size) {
    // Aloca + deref en una llamada: devuelve el host_ptr al payload (estable en
    // v1 no-moving).  Lo usa el helper __new_<X>_gc del frontend (gc<T>): el ptr
    // se guarda en el slot del var-decl, marcado HOSTPTR en el stackmap ->
    // handle_for_ptr lo resuelve al handle en la coleccion.
    gc::GcHeap &h = gc_heap();
    const gc::GcHandle handle = h.alloc_pinned(static_cast<size_t>(size));
    if (handle == gc::GC_NULL_HANDLE) return nullptr;
    return h.deref(handle);
}

void vex_gc_pin(uint32_t handle) {
    gc_heap().gc_addref(static_cast<gc::GcHandle>(handle));
}

void vex_gc_unpin(uint32_t handle) {
    gc_heap().gc_release(static_cast<gc::GcHandle>(handle));
}

uint8_t *vex_gc_deref(uint32_t handle) {
    return gc_heap().deref(static_cast<gc::GcHandle>(handle));
}

void vex_gc_collect(void) {
    gc_heap().minor_gc();
    gc_heap().major_gc();
}

uint64_t vex_gc_live_count(void) {
    // No hay accesor directo de "handles vivos"; lo contamos sobre la tabla
    // (O(N), solo para introspeccion/diagnostico, no es hot path).
    gc::GcHeap &h = gc_heap();
    uint64_t n = 0;
    const size_t cap = h.handle_table_size();
    for (size_t i = 0; i < cap; ++i)
        if (h.is_handle_live(static_cast<gc::GcHandle>(i))) ++n;
    return n;
}

} // extern "C"
