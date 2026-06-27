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
    return static_cast<uint32_t>(gc_heap().alloc(static_cast<size_t>(size)));
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
