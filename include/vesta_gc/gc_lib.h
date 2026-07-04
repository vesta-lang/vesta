/**
 * @file vesta_gc/gc_lib.h
 * @brief C-ABI de `libvesta_gc` -- el GC opt-in de Vesta para codigo nativo AOT.
 *
 * `gc<T>` de Vesta (opt-in via `import vex.gc`) baja a CALLs a estas funciones,
 * igual que vex_io / vex_mem.  El motor por debajo es EL MISMO `gc::GcHeap` que
 * usan el interprete y el JIT (generacional mark-sweep + handle table) -> el
 * comportamiento es UNIFORME en los tres backends por construccion, sin
 * duplicar implementacion.
 *
 * @par Modelo en AOT
 * Un unico `GcHeap` global (sin `ProcessVM`: no hay vm_mem ni proceso).  Las
 * raices se descubren con STACKMAPS PRECISOS emitidos por el codegen AOT en cada
 * safepoint: `major_gc` camina los frames NATIVOS (cadena RBP) y, por cada
 * direccion de retorno, lee de su stackmap los slots/registros vivos que
 * contienen un GcHandle (reusa `scan_jit_roots_precise`, el mismo walker que el
 * JIT).  Permite el GC generacional moving completo (no conservativo).
 *
 * @par Enlazado
 * El default JAMAS referencia estos simbolos -> cero GC enlazado salvo opt-in.
 * Cuando el programa usa `gc<T>`, el driver `-m aot` auto-enlaza `libvesta_gc`
 * (via el linker propio AOT.5).
 */
#ifndef VESTA_GC_GC_LIB_H
#define VESTA_GC_GC_LIB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa el heap global del GC.  Idempotente (no-op tras la 1a vez).
 * @note Lo llama el arranque del binario AOT antes del primer `vex_gc_alloc`.
 *       Llamarlo de mas es seguro.
 */
void vex_gc_init(void);

/**
 * @brief Aloca un objeto GC de @p size bytes de payload.
 * @param size Tamanño del payload en bytes (sin el GcHeader).
 * @return GcHandle del nuevo objeto (indice estable en la handle table).  Usa
 *         @c vex_gc_deref para obtener el puntero al payload.
 */
uint32_t vex_gc_alloc(uint64_t size);

/**
 * @brief Traduce un GcHandle a su puntero de payload.
 * @param handle GcHandle devuelto por @c vex_gc_alloc.
 * @return Puntero host al payload, o NULL si el handle no es valido/esta muerto.
 * @note Estable hasta el proximo GC (el GC puede mover el objeto; el handle
 *       sigue siendo valido, el puntero crudo NO -> re-derefa tras un safepoint).
 */
uint8_t *vex_gc_deref(uint32_t handle);

/**
 * @brief Fuerza un ciclo de coleccion (minor + major).
 * @note Tambien se dispara automaticamente cuando el nursery/old se llena.
 */
void vex_gc_collect(void);

/**
 * @brief Aloca un objeto GC de @p size bytes y devuelve su host_ptr (aloca +
 *        deref).  Lo usa el helper `__new_<X>_gc` del frontend.
 * @param size Tamanño del payload en bytes.
 * @return host_ptr al payload (estable en v1 no-moving), o NULL si OOM.
 */
uint8_t *vex_gc_alloc_ptr(uint64_t size);

/**
 * @brief Registra los stackmaps AOT (seccion .vexgc_smap) en el JitRegistry
 *        para que el scan preciso descubra raices gc<T> en los frames nativos.
 * @param sec  Puntero al inicio de la seccion .vexgc_smap (el tamanño total va
 *             embebido en su header, offset 12).
 * @note La llama el arranque del binario (CALL __vexgc_init en main) antes del
 *       primer alloc.  Idempotente-ish: registrar dos veces duplica entradas
 *       (el arranque la llama una sola vez).
 */
void vex_gc_register_aot_stackmaps(const uint8_t *sec);

/**
 * @brief Pinna un handle como raiz externa (refcount): no se colecta mientras
 *        este pinnado.  Usado para raices que el GC no ve via stackmaps
 *        (globals nativos, estructuras host) y en tests.
 * @param handle GcHandle a pinnar.
 */
void vex_gc_pin(uint32_t handle);

/**
 * @brief Quita un pin de raiz externa (decrementa el refcount).
 * @param handle GcHandle a despinnar.
 */
void vex_gc_unpin(uint32_t handle);

/**
 * @brief Numero de handles vivos (introspeccion/diagnostico).
 * @return Cantidad de objetos GC actualmente vivos.
 */
uint64_t vex_gc_live_count(void);

/**
 * @brief Registra un finalizador GC para un objeto AOT con recurso interno.
 *
 * Cuando el sweep colecte el objeto (o el shutdown lo finalice), el runner
 * NATIVO invocara el deleter/dtor concreto por CALL directo (el codigo esta
 * AOT-compilado, cero bytecode).  Cierra la fuga del escape en AOT: un
 * gc<unique>/gc<shared>/gc<Clase> que escapa su scope libera su recurso
 * exactamente una vez.
 *
 * @param payload host_ptr al payload del box (lo que ve el programa).
 * @param kind    1=UNIQUE (box=[inner_ptr@0, deleter@8]), 2=SHARED
 *                (box=[ctrl@0]), 3=CLASS_DTOR (payload=instancia).
 * @param aux     CLASS_DTOR: func_ptr nativo del <Clase>____dtor.  Ignorado
 *                para UNIQUE/SHARED (su deleter vive dentro del box).
 */
void vex_gc_register_finalizer(uint8_t *payload, uint32_t kind, uint64_t aux);

/**
 * @brief Desregistra el finalizador de un objeto (anti-doble-free desde el
 *        cleanup determinista de scope, caso no-escape).
 * @param payload host_ptr al payload del box.
 */
void vex_gc_unregister_finalizer(uint8_t *payload);

/**
 * @brief Finaliza TODO objeto GC vivo con recurso interno (shutdown-time).
 *
 * Lo llama el epilogo del binario AOT (atexit / fin de main) para garantizar
 * cero fuga: los objetos escapados que el sweep no colecto todavia corren su
 * deleter/dtor antes de salir.  Idempotente.
 */
void vex_gc_finalize_all(void);

/**
 * @brief Numero de finalizadores nativos ejecutados (introspeccion/diagnostico).
 * @return Contador acumulado de deleters/dtors invocados por el runner nativo.
 */
uint64_t vex_gc_fin_count(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // VESTA_GC_GC_LIB_H
