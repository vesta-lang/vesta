/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file type_classify.h
 * @brief Clasificador de tipos para la frontera C / ownership de structs.
 *
 * Fase 0 del plan de interop C + ownership (ver
 * doc/VMdoc/Vesta/InteropC.md y la memoria del proyecto).  Define dos
 * predicados PUROS sobre un @c Type (con un resolver de layouts de struct
 * para recursar sobre campos).  NO tocan codegen: son analisis estatico que
 * alimentan la inferencia de structs (Fase 1), el move-checker (Fase 3) y la
 * generacion de headers .h (Fase 4).
 *
 * Los dos carriles del lenguaje (decididos por el TIPO, sin anotaciones):
 *
 *   - C-representable: cruza la frontera C por valor con ABI C, copiable como
 *     bits, sin direccion del espacio VM y sin lifetime gestionado.  Es el
 *     carril por defecto (POD, cfn, punteros host, structs C-compat).  El
 *     layout C de los structs es un INVARIANTE del lenguaje (orden de
 *     declaracion + alineacion de plataforma), no una anotacion @repr(C).
 *
 *   - Gestionado: posee/transporta un recurso de lifetime no-C (env de
 *     closure, string/clase/coleccion GC, unique/shared, o un struct con
 *     alguno de esos o un destructor `~Struct()`).  Es el carril move-only +
 *     RAII (interno; cruza la frontera C solo descompuesto, p.ej. un closure
 *     como `cfn + void*`).
 *
 * NOTA: los dos predicados NO son complementarios.  Un @c VirtualPtr<T>
 * (direccion del espacio VM) no es C-representable PERO tampoco es gestionado
 * (es value-copiable, sin cleanup) -- responde a preguntas distintas.
 */

#ifndef VX_TYPE_CLASSIFY_H
#define VX_TYPE_CLASSIFY_H

#include <functional>
#include <string>

namespace vx {

struct Type;         ///< fwd (definido en vx/types.h)
struct StructLayout; ///< fwd (definido en vx/type_checker.h)

/**
 * @brief Resolver de layouts de struct para la recursion sobre campos.
 * @param name nombre del struct (Type::struct_name).
 * @return el @c StructLayout, o @c nullptr si no resuelve (defensivo: un
 *         struct no resuelto se trata como NO C-representable y NO gestionado).
 */
using StructResolver = std::function<const StructLayout *(const std::string &)>;

/**
 * @brief Es el tipo representable en C (cruza la frontera por valor, ABI C)?
 *
 * Reglas (recursivas en STRUCT/ARRAY/OPTIONAL/RESULT):
 *  - primitivos (void/bool/char/i8..i64/u8..u64/f32/f64) -> si.
 *  - PTR host (is_virtual=false) -> si (puntero de 8 bytes; el pointee puede
 *    ser cualquier cosa: C lo ve opaco).  VirtualPtr (is_virtual=true) -> no
 *    (es una direccion del espacio VM, sin sentido para C).
 *  - ARRAY -> si el elemento es C-representable (array C inline).
 *  - FUNCTION: cfn (fn_is_raw=true) -> si (puntero a funcion C de 8 bytes);
 *    lambda (fn_is_raw=false) -> no (su env es gestionada).
 *  - STRUCT -> si TODOS sus campos son C-representables Y no tiene `~Struct()`.
 *  - BORROW/BORROW_MUT -> si (en runtime es un host_ptr T* de 8 bytes).
 *  - OPTIONAL/RESULT -> si sus payloads son C-representables (POD etiquetado).
 *  - CLASS, STRING, colecciones, FUTURE, UNIQUE/SHARED, GC_PTR, TYPE_META -> no.
 *  El @c nominal_id (newtype) no altera la representabilidad (mismos bits).
 */
bool is_c_representable(const Type &t, const StructResolver &find_struct);

/**
 * @brief Posee/transporta el tipo un recurso de lifetime gestionado?
 *
 * Determina el carril move-only + RAII.  Reglas:
 *  - FUNCTION lambda (fn_is_raw=false) -> si (posee env; conservador a nivel
 *    de tipo: no sabemos el conteo de capturas, asumimos potencialmente owned).
 *  - STRING, CLASS, colecciones (ARRAYLIST..STACK), FUTURE, UNIQUE_PTR,
 *    SHARED_PTR, GC_PTR -> si.
 *  - STRUCT -> si tiene algun campo gestionado O un destructor `~Struct()`.
 *  - ARRAY -> si su elemento es gestionado.
 *  - OPTIONAL/RESULT -> si algun payload es gestionado.
 *  - primitivos, cfn, PTR (host o virtual), BORROW/BORROW_MUT -> no
 *    (el puntero en si no posee; el cleanup, si lo hay, es del apuntado).
 */
bool is_managed(const Type &t, const StructResolver &find_struct);

} // namespace vx

#endif // VX_TYPE_CLASSIFY_H
