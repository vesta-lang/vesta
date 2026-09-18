/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file vx/ufcs.h
 * @brief Llamada uniforme: `x.f(a)` y `f(x, a)` son la MISMA llamada.
 *
 * @par Por que un modulo y no unas lineas en el comprobador
 * Lo que hay aqui es una REGLA del lenguaje con su propia estructura de datos,
 * no un caso mas de la resolucion de metodos.  Repartirla por el comprobador
 * -- unas lineas donde falla un metodo de struct, otras donde falla el de una
 * clase, y la tabla en un miembro suyo -- deja el criterio en varios sitios a
 * la espera de divergir, que es exactamente lo que ya paso con el simbolo de un
 * metodo.  El precedente es @c vx::overload: la regla vive en un sitio y quien
 * la necesita la LLAMA.
 *
 * @par Que decide, y que no
 * Aqui se decide QUE candidatas hay para un receptor y un nombre.  Elegir entre
 * ellas es @c overload::select -- la misma que usa una llamada libre, a
 * proposito: si aqui se decidiera de otra manera, las dos grafias dejarian de
 * ser la misma llamada --, y reescribir el arbol es del comprobador.
 *
 * @par La estructura
 * La correlacion se hace al DECLARAR, no al llamar: al declarar `area(Punto p)`
 * se calcula la clave una vez y se inserta.  La clave es la CABEZA del tipo del
 * primer parametro, no el tipo entero, para que lo declarado contra `Caja<T>`
 * lo encuentre un receptor `Caja<i64>`; y los dos trozos de la clave son
 * punteros ya internados, asi que en el sitio de llamada no se hashea texto ni
 * se arma ninguna cadena.
 */

#ifndef VX_UFCS_H
#define VX_UFCS_H

#include "util/alloc/small_vector.h"
#include "vx/types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vx {
namespace ufcs {

/// Las candidatas de un (cabeza, nombre).  Son pocas: un punyado por nombre.
using Candidates = util::SmallVector<uint32_t, 4>;

/**
 * @brief La CABEZA del tipo: por lo que se indexa.
 *
 * Si la clave fuera el nombre canonico entero, una declaracion escrita contra
 * `Caja<T>` no la encontraria nunca un receptor de tipo `Caja<i64>`: son dos
 * cadenas distintas.  Asi que se indexa por el CONSTRUCTOR del tipo.
 *
 * | tipo del receptor | cabeza |
 * | :-- | :-- |
 * | `i64`, `u8`, `f64`, `char`, `bool` | `num` |
 * | `i64*` | `ptr` |
 * | `Punto[8]` | `array` |
 * | `Caja<i64>` | `Caja` |
 *
 * Los escalares comparten cubo por la misma razon, un escalon mas abajo: una
 * llamada libre acepta `add(y, 3)` con `y : i32` para `add(u64, u64)`, y un
 * literal sin sufijo se re-tipa si cabe.  Con la cabeza exacta, `y.add(3)` y
 * `6.doble()` no encontrarian la candidata que `add(y, 3)` y `doble(6)` si
 * encuentran, y las dos grafias dejarian de ser la misma llamada.
 *
 * Eso vale para los PRIMITIVOS del lenguaje y solo para ellos: un tipo FUERTE
 * (`typedef u32 Edad new`) es por dentro un entero pero tiene cubo propio, que
 * es precisamente lo que se declara al hacerlo fuerte.
 *
 * @param t El tipo.
 * @return Su cabeza, INTERNADA: la comparacion es de punteros.
 */
const std::string *head_of(const Type &t);

/**
 * @struct Index
 * @brief Lo declarado en este modulo, listo para preguntarle por un receptor.
 */
struct Index {
    /**
     * @brief Apunta una funcion libre: su primer parametro manda.
     *
     * Una sin parametros no entra -- no hay receptor posible --, y tampoco hace
     * falta filtrarla despues: no esta.
     *
     * @param first_param Tipo de su primer parametro.
     * @param name        Nombre con el que se declaro.
     * @param slot        Como la reconoce quien pregunta (su indice de firma).
     */
    void declare(const Type &first_param, const std::string &name,
                 uint32_t slot);

    /**
     * @brief Apunta que @p param tambien puede recibir el receptor, por hueco.
     *
     * Con `_` el receptor no cae en el primero -- `"hola".sumar(.a = 3, .b = _)`
     * lo manda a `b` --, asi que buscar solo por la cabeza del PRIMER parametro
     * no encuentra nada.  Esto vive en su propia tabla y no mezclado con la
     * otra: la regla 2.2 pregunta por "quien podria ser el metodo de esto", que
     * es el primero y nada mas, y meter aqui los demas la haria gritar por
     * funciones que solo se alcanzan escribiendo un hueco.
     *
     * @param param Tipo de UN parametro suyo (el primero incluido).
     * @param name  Nombre con el que se declaro.
     * @param slot  Como la reconoce quien pregunta.
     */
    void declare_any(const Type &param, const std::string &name,
                     uint32_t slot);

    /**
     * @brief Como @c find, pero mirando CUALQUIER parametro.
     *
     * Solo para una llamada con hueco.  Sin hueco la pregunta es otra -- "cual
     * puede tomar esto de PRIMERO" -- y la contesta @c find.
     */
    const Candidates *find_any(const Type &recv, const std::string &written,
                               const std::string &site_prefix,
                               const std::string **matched = nullptr) const;

    /**
     * @brief Las que podrian tomar @p recv con ese nombre, o nulo si ninguna.
     *
     * No dice CUAL: eso lo decide @c overload::select con los argumentos, que
     * es la misma regla que usa una llamada libre.
     *
     * Prueba el nombre tal y como se escribio y, si no, el mismo con el
     * prefijo del namespace DESDE EL QUE SE LLAMA -- un `x.f()` lleva `f` a
     * secas y la funcion se declaro `<ns>__f` --.
     *
     * Solo ESE prefijo, y no los demas que el fichero declare.  Probarlos
     * todos hacia que el punto alcanzara un namespace que no esta en ambito:
     * con dos `doble` en dos namespaces del mismo fichero, `6.doble()` se iba
     * a la PRIMERA -- eligiendo por el orden de dos lineas que nadie mira --
     * mientras que `doble(6)`, que es la misma llamada escrita del otro modo,
     * decia que no estaba declarada.  Lo que el punto encuentra y lo que
     * encuentra la llamada libre tienen que ser lo mismo.
     *
     * @param recv        El tipo del receptor.
     * @param written     El nombre escrito tras el punto.
     * @param site_prefix Prefijo del namespace donde esta la llamada (`app__`),
     *                    o vacio en la raiz.
     * @param matched     Si no es nulo, recibe el nombre que acerto, INTERNADO
     *                    -- que es el que hay que escribir al reescribir la
     *                    llamada.
     */
    const Candidates *find(const Type &recv, const std::string &written,
                           const std::string &site_prefix,
                           const std::string **matched = nullptr) const;

    /**
     * @brief TODAS las que se llaman asi, sea cual sea el receptor.
     *
     * Para el DIAGNOSTICO, no para resolver: cuando no hay ninguna para ESTE
     * receptor, poder decir para cuales si las hay y con que cast se llega.
     * Sin esto el mensaje solo sabe negar -- "no hay ninguna que tome un
     * `i64`" -- cuando la candidata esta ahi al lado pidiendo un `Edad`.
     *
     * Es una SONDA, no un recorrido: hay una segunda tabla por nombre, que es
     * la del 6-bis.3 y crece con las declaraciones igual que la otra -- cuatro
     * bytes mas por funcion y una entrada por nombre distinto --.  Recorrer el
     * indice entero seria O(declaraciones del programa) por error, y con la
     * stdlib dentro eso no es "solo cuando falla", es medio segundo.
     *
     * @param written El nombre escrito tras el punto.
     * @param matched Si no es nulo, recibe el nombre con el que se declararon,
     *                INTERNADO -- que es de donde sale a que namespace
     *                pertenecen.
     * @return Las candidatas con ese nombre, o nulo si no hay ninguna.
     */
    const Candidates *all_named(const std::string &written,
                                const std::string **matched = nullptr) const;

    /**
     * @brief Apunta que @p mangled es como el aplanado escribio @p public_name.
     *
     * Un `x.f()` lleva `f` tal cual -- el aplanado renombra declaraciones y
     * sitios de llamada, no lo que va tras un punto --, mientras que la funcion
     * se declaro como `<ns>__f`.  Sin esto, buscar `f` no encontraria nada en
     * cuanto el fichero declara un namespace, que es casi siempre.
     *
     * Se guarda el PREFIJO, no el par: son unos pocos por modulo y asi la
     * llamada no recorre nada.
     */
    void note_flattened(const std::string &mangled,
                        const std::string &public_name);

  private:
    /// (cabeza, nombre) -> candidatas.  Crece con las DECLARACIONES del
    /// programa, no con el uso: no es el producto cruzado tipo x funcion, que
    /// es lo que impediria que esto se dispare.
    struct Key {
        const std::string *head;
        const std::string *name;
        bool operator==(const Key &o) const noexcept {
            return head == o.head && name == o.name;
        }
    };
    struct KeyHash {
        size_t operator()(const Key &k) const noexcept {
            /* Los dos son punteros del pozo, asi que mezclarlos basta: no se
             * vuelve a hashear el texto. */
            const size_t a = reinterpret_cast<size_t>(k.head);
            const size_t b = reinterpret_cast<size_t>(k.name);
            return a * 1099511628211ull ^ (b + 0x9e3779b97f4a7c15ull + (a << 6));
        }
    };
    std::unordered_map<Key, Candidates, KeyHash> by_head_;
    /// Lo mismo por la cabeza de CUALQUIER parametro, para la llamada con
    /// hueco.  Aparte de  by_head_ a proposito: ver  declare_any.
    std::unordered_map<Key, Candidates, KeyHash> by_any_;
    /// Lo MISMO indexado solo por nombre, para poder decir para que receptores
    /// SI la hay cuando no la hay para este.  Es la segunda tabla de 6-bis.3:
    /// crece con las declaraciones, no con el uso.
    struct NameHash {
        size_t operator()(const std::string *n) const noexcept {
            return reinterpret_cast<size_t>(n) * 1099511628211ull;
        }
    };
    std::unordered_map<const std::string *, Candidates, NameHash> by_name_;
    std::vector<std::string> prefixes_;
};

} // namespace ufcs
} // namespace vx

#endif // VX_UFCS_H
