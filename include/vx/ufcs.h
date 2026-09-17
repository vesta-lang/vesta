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
 * | `i64` | `i64` |
 * | `i64*` | `ptr` |
 * | `Punto[8]` | `array` |
 * | `Caja<i64>` | `Caja` |
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
     * @brief Las que podrian tomar @p recv con ese nombre, o nulo si ninguna.
     *
     * No dice CUAL: eso lo decide @c overload::select con los argumentos, que
     * es la misma regla que usa una llamada libre.
     *
     * Prueba el nombre tal y como se escribio y, si no, el mismo con cada
     * prefijo que el aplanado use en este modulo -- un `x.f()` lleva `f` a
     * secas y la funcion se declaro `<ns>__f` --.  El bucle vive aqui y no en
     * quien pregunta: es parte de con que nombre se declaro cada cosa.
     *
     * @param recv    El tipo del receptor.
     * @param written El nombre escrito tras el punto.
     * @param matched Si no es nulo, recibe el nombre que acerto, INTERNADO --
     *                que es el que hay que escribir al reescribir la llamada.
     */
    const Candidates *find(const Type &recv, const std::string &written,
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
    std::vector<std::string> prefixes_;
};

} // namespace ufcs
} // namespace vx

#endif // VX_UFCS_H
