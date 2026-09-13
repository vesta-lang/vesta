/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file overload.h
 * @brief La SOBRECARGA: varias declaraciones con un nombre, y como se elige.
 *
 * Un solo sitio para las dos preguntas que aparecen cada vez que el lenguaje
 * deja repetir un nombre:
 *
 *   1. **Cual de ellas quiere esta llamada** -- @c select, la regla de
 *      preferencia.
 *   2. **Como se llama el simbolo de cada una** -- @c discriminator, lo que las
 *      separa en el fichero objeto.
 *
 * Las dos valen igual para una funcion libre, para un metodo de struct o de
 * clase y para un constructor.  Escritas una vez, no hay forma de que una de
 * las copias elija distinto que otra -- que es precisamente lo que pasaba
 * cuando el tipo de retorno salia de cinco tablas indexadas por nombre.
 *
 * @par Lo que esto NO reserva
 * Nada.  Las candidatas se APUNTAN, no se copian; no hay tabla intermedia, ni
 * cadenas construidas para preguntar, ni @c std::function.  La compatibilidad
 * entra por puntero a funcion, asi que el comprobador presta sus reglas sin
 * que este modulo dependa de el.  Es deliberado: elegir sobrecarga es raro,
 * pero la ruta por la que se pregunta la pisan TODAS las llamadas del
 * programa.
 */

#ifndef VX_OVERLOAD_H
#define VX_OVERLOAD_H

#include "vx/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vx {

/// @namespace vx::overload
/// @brief La regla de la sobrecarga, comun a funciones, metodos y
///        constructores.
namespace overload {

/// Nadie encaja.
constexpr uint32_t kNoPick = 0xFFFFFFFFu;

/**
 * @struct Candidate
 * @brief Una de las que comparten el nombre.
 *
 * @c params se APUNTA: la lista vive en la firma o en la ficha del metodo, y
 * copiarla para preguntar seria reservar en la unica parte cara de esto.
 * @c slot es como el que pregunta reconoce a la elegida -- un indice en su
 * propia tabla --, asi que este modulo no necesita saber que hay dentro.
 */
struct Candidate {
    const std::vector<Type> *params = nullptr;
    uint32_t slot = kNoPick;
    /**
     * @brief Que parametros viajan por REFERENCIA (un bit por posicion).
     *
     * Uno de SALIDA se declara `T` y viaja como `T*`, asi que el argumento hay
     * que compararlo contra lo APUNTADO: quien llama cede el hueco, no su
     * direccion.  Sin esto, una funcion con `out` se puede declarar y no hay
     * forma de llamarla -- ninguna candidata encaja --.
     *
     * Lo hacia solo el constructor de un struct, por su cuenta; aqui vale para
     * todos, que es donde debia estar.
     */
    uint64_t by_ref_mask = 0;
};

/**
 * @brief Si un argumento de tipo @p arg vale para un parametro de tipo
 *        @p param, ADMITIENDO conversion.
 *
 * La presta el comprobador, que es quien sabe de asignabilidad, herencia e
 * interfaces.  Puntero a funcion con contexto y no un metodo virtual: no hay
 * que construir ningun objeto para preguntar, y la llamada indirecta solo se
 * paga en la pasada permisiva, que es la rara.
 */
using AcceptsFn = bool (*)(void *ctx, const Type &param, const Type &arg);

/**
 * @brief La que piden estos argumentos.
 *
 * @par El orden de preferencia, y por que es este
 * DOS PASADAS: primero la que encaja EXACTA, y solo si no hay ninguna, la
 * primera que admita conversion.  Con una sola pasada `f(2.0)` se iria a
 * `f(i64)` -- un f64 es asignable a un i64 -- y nunca llegaria a la de f64, que
 * es la que el usuario escribio.  Es el mismo orden que el lenguaje ya usa al
 * especializar un generico: exacta, luego patron, luego la primaria.
 *
 * Un argumento que no se pudo tipar (@c PrimitiveKind::COUNT) no descarta a
 * nadie: su error ya esta dado, y descartar por el solo anyadiria un segundo
 * mensaje diciendo que la funcion no existe.
 *
 * @param cands   Las que comparten nombre, en el orden en que se declararon.
 * @param n       Cuantas son.
 * @param args    Los tipos de los argumentos de la llamada.
 * @param accepts La regla de conversion, del comprobador.
 * @param ctx     Lo que @p accepts necesite; se le pasa tal cual.
 * @return El @c Candidate::slot de la elegida, o @c kNoPick.
 */
uint32_t select(const Candidate *cands, size_t n, const std::vector<Type> &args,
                AcceptsFn accepts, void *ctx);

/**
 * @brief Lo que separa el simbolo de una sobrecarga del de sus hermanas.
 *
 * Sus parametros, con el MISMO mangleado que ya usan los genericos: una sola
 * forma de nombrar en todo el compilador, no dos.
 *
 * Solo se llama para las que de verdad estan sobrecargadas.  **Una que no lo
 * este conserva su simbolo exacto**, que es el que ven el enlazador, `@Export`,
 * la FFI y la reflexion; cambiarselo por uniformidad romperia a todos ellos
 * para arreglar a nadie.
 *
 * @param params Los parametros de esa candidata.
 */
std::string discriminator(const std::vector<Type> &params);

/**
 * @brief Cuantas de @p cands comparten los MISMOS parametros que @p which.
 *
 * Sirve para las dos preguntas que se hacen a la vez al declarar: repetir
 * nombre y parametros es una REDEFINICION (error), y repetir solo el nombre es
 * una sobrecarga (legitima).
 *
 * Sin tabla y sin cadenas: comparar dos listas de tipos no reserva, y las
 * candidatas de un nombre son de un digito.
 */
bool same_params(const std::vector<Type> &a, const std::vector<Type> &b);

} // namespace overload
} // namespace vx

#endif // VX_OVERLOAD_H
