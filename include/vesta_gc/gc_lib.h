/**
 * @file vesta_gc/gc_lib.h
 * @brief C-ABI de `libvesta_gc` -- el GC opt-in de Vex para codigo nativo AOT.
 *
 * `gc<T>` de Vex (opt-in via `import vex.gc`) baja a CALLs a estas funciones,
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

#ifdef __cplusplus
} // extern "C"
#endif

#endif // VESTA_GC_GC_LIB_H
