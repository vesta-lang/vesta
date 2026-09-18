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
    /**
     * @brief Como se llaman sus parametros, alineado con @c params.
     *
     * Solo hace falta cuando la llamada nombra alguno (`f(.a = 3)`), y entonces
     * el nombre entra en la SELECCION: una candidata que no tenga un parametro
     * `a` no es viable, aunque los tipos cuadraran.  Nulo cuando no se sabe.
     */
    const ParamNames *param_names = nullptr;
    /**
     * @brief Hay otra que toma LO MISMO y solo se distinguen por los nombres.
     *
     * Un bit, no una busqueda: lo apunto el recorrido de hermanas al
     * declararlas.  Sirve para no pagar nada en el caso normal -- si esto es
     * falso, la primera que encaja ES la respuesta, como siempre -- y para
     * mirar si hay una segunda solo cuando de verdad puede haberla.
     */
    bool needs_names = false;
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
    /**
     * @brief El tipo del ELEMENTO si la ultima posicion es variadica; nulo si
     *        toma un numero exacto de argumentos.
     *
     * Un puntero y no un `bool` mas un tipo: la presencia ES el hecho, asi que
     * no pueden contradecirse.
     *
     * Sin esto, una variadica no competia -- la seleccion pedia que la lista
     * midiera EXACTAMENTE lo que la llamada, y la suya mide uno --, asi que
     * `f(i64... xs)` dejaba de aceptar cualquier cantidad en cuanto se le
     * declaraba un hermano con su nombre: la MISMA llamada que compilaba antes
     * de existir el hermano pasaba a ser un error de aridad.
     */
    const Type *variadic_elem = nullptr;
    /**
     * @brief Aridad abierta SIN tipo de elemento: el `...` crudo de una
     *        `@Naked`, cuyo cuerpo lee los registros del ABI a mano.
     *
     * Es OTRA cosa que el variadico con tipo, y confundirlos no da un error
     * sino un acceso invalido: el crudo no anyade parametro -- asi que tomar
     * "todos menos el ultimo" como fijos se come uno de verdad -- y no tiene
     * tipo de elemento contra el que comparar, asi que preguntar por el es
     * preguntar por un tipo sin rellenar.  Aqui acepta de su cuenta de
     * parametros en adelante y lo que sobra no se comprueba, que es lo que el
     * lenguaje promete de un `...` crudo.
     */
    bool raw_variadic = false;
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
 * @par Argumentos con NOMBRE
 * `f(.a = 3, .b = 1)`.  La lista deja de estar en orden, y reordenarla es POR
 * CANDIDATA -- cada sobrecarga puede llamar distinto a sus parametros --, asi
 * que se hace AQUI y no en quien pregunta: hacerlo fuera obligaria a repetir la
 * regla en cada sitio de llamada, que son cinco.  Sin nombres no cuesta nada:
 * una sonda a un puntero nulo y el camino de siempre.
 *
 * @param cands   Las que comparten nombre, en el orden en que se declararon.
 * @param n       Cuantas son.
 * @param args    Los tipos de los argumentos de la llamada.
 * @param accepts La regla de conversion, del comprobador.
 * @param ctx     Lo que @p accepts necesite; se le pasa tal cual.
 * @param arg_names Con que nombre se escribio cada argumento, alineado con
 *                @p args y vacio donde fue posicional.  Nulo o vacio = ninguno
 *                lleva nombre, que es el caso normal.
 * @return El @c Candidate::slot de la elegida, o @c kNoPick.
 */
uint32_t select(const Candidate *cands, size_t n, const std::vector<Type> &args,
                AcceptsFn accepts, void *ctx,
                const ParamNames *arg_names = nullptr,
                uint32_t *other_fit = nullptr);

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
std::string discriminator(const std::vector<Type> &params,
                          const ParamNames *names = nullptr);

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

/**
 * @brief La MISMA firma: mismos tipos y las ranuras llamadas igual.
 *
 * Desde que una llamada puede nombrar la ranura (`f(.a = 3)`), dos que toman lo
 * mismo pero lo llaman distinto SON distinguibles, asi que son dos y no una
 * repetida.  Con esto se puede escribir
 *
 * ```vx
 * i64 mide(i64 alto, i64 ancho)
 * i64 mide(i64 largo, i64 grosor)
 * ```
 *
 * que antes era "redefinicion".  Basta con que UNA ranura se llame distinto.
 *
 * Lo que arrastra, y no es opcional: **el simbolo tiene que llevar lo que las
 * separa**, o las dos acaban con la misma etiqueta -- dos cuerpos, un nombre
 * --, que es el fallo que ya mordio aqui con la tabla de metodos.  Ver
 * @c discriminator.
 *
 * Una lista de nombres VACIA (no se supo) no separa: dos asi siguen siendo la
 * misma, que es lo conservador.
 */
bool same_signature(const std::vector<Type> &ta, const ParamNames &na,
                    const std::vector<Type> &tb, const ParamNames &nb);

} // namespace overload
} // namespace vx

#endif // VX_OVERLOAD_H
