/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file util/named_alloc.h
 * @brief Poner NOMBRE a un contenedor para que el perfil diga que es.
 *
 * POR QUE.  Medido con el modo sanitizador sobre 441.089 lineas, compilar hace
 * **116 millones de reservas** desde 1.194 sitios, y **el 60% de ellas no se
 * puede atribuir a codigo nuestro**: el paseo de la pila se queda dentro de
 * libstdc++ y el informe solo sabe decir `std::vector<unsigned int>`.  Con eso
 * en la mano no se puede arreglar nada, porque hay DECENAS de estructuras
 * auxiliares distintas que son, byte a byte, el mismo tipo.
 *
 * Y un alias NO lo resuelve: `using IrValueId = uint32_t` es transparente al
 * sistema de tipos y al mangling, asi que `std::vector<IrValueId>` se llama
 * `std::vector<unsigned int>` igual que los demas.  Para que el proposito
 * viaje en el SIMBOLO tiene que ser un tipo DISTINTO.
 *
 * COMO.  El unico parametro de plantilla que `std::vector` ya lleva y nadie
 * usa es el asignador, asi que la etiqueta va ahi.  @ref NamedAlloc declara su
 * PROPIA `allocate` -- no la hereda de `std::allocator` -- para que el nombre
 * de esa funcion, etiqueta incluida, aparezca en la cadena de llamadas:
 *
 *     util::NamedAlloc<unsigned int, ir::scratch::DceUsed>::allocate(...)
 *
 * Sigue apareciendo aunque el compilador la inline, porque la cadena guarda
 * tambien los marcos inlinados, y sobrevive a una llamada de cola, que es
 * donde el paseo de la pila miente (ver la memoria de sitios de llamada de
 * cola).  Es identificacion por TIPO, no por direccion de retorno.
 *
 * COSTE CERO.  Sin estado, sin miembros, y reserva por `::operator new`, que es
 * exactamente lo que hace `std::allocator`: el mismo camino y la misma
 * contabilidad del asignador del anfitrion.  Lo unico que cambia es el nombre.
 *
 * LO QUE CUESTA DE VERDAD, y es a proposito: `NamedVector<T, Tag>` NO es
 * `std::vector<T>`, asi que no se le pasa a una firma que pida el segundo.  Eso
 * limita su uso a estructura auxiliar LOCAL -- que es justo donde viven los 116
 * millones -- y convierte en error de compilacion el intento de sacarla de ahi,
 * que es el modo de fallo bueno.
 */
#ifndef VESTA_UTIL_NAMED_ALLOC_H
#define VESTA_UTIL_NAMED_ALLOC_H

#include <cstddef>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace util {

/**
 * @brief Asignador sin estado cuyo NOMBRE dice para que es la memoria.
 * @tparam T   Tipo del elemento.
 * @tparam Tag Etiqueta; basta con declararla, no hace falta definirla.
 */
template <typename T, typename Tag> struct NamedAlloc {
    /// Lo exige `std::allocator_traits`.
    using value_type = T;

    NamedAlloc() noexcept = default;

    /// Conversion entre etiquetas IGUALES y tipos distintos: la necesitan los
    /// contenedores basados en nodos, que reservan nodos, no elementos.
    template <typename U> NamedAlloc(const NamedAlloc<U, Tag> &) noexcept {}

    /// Rebind explicito: el implicito de `allocator_traits` tambien acertaria,
    /// pero dejarlo escrito evita depender de esa deduccion.
    template <typename U> struct rebind {
        using other = NamedAlloc<U, Tag>;
    };

    /**
     * @brief Reserva sitio para @p n elementos.
     *
     * ESTA es la funcion que da nombre al sitio en el perfil, y por eso se
     * escribe aqui en vez de heredarla: heredada, la cadena de llamadas diria
     * `std::allocator<unsigned int>::allocate` y estariamos donde empezamos.
     *
     * @param n Cuantos elementos.
     * @return Memoria sin construir para @p n elementos.
     */
    T *allocate(std::size_t n) {
        return static_cast<T *>(::operator new(n * sizeof(T)));
    }

    /**
     * @brief Suelta lo que dio @ref allocate.
     * @param p Puntero devuelto por @ref allocate.
     */
    void deallocate(T *p, std::size_t) noexcept { ::operator delete(p); }
};

/// Dos asignadores con la MISMA etiqueta son intercambiables: no tienen estado,
/// asi que cualquiera puede soltar lo que reservo el otro.
template <typename T, typename U, typename Tag>
bool operator==(const NamedAlloc<T, Tag> &,
                const NamedAlloc<U, Tag> &) noexcept {
    return true;
}

template <typename T, typename U, typename Tag>
bool operator!=(const NamedAlloc<T, Tag> &,
                const NamedAlloc<U, Tag> &) noexcept {
    return false;
}

/// Un vector que dice que es.
template <typename T, typename Tag>
using NamedVector = std::vector<T, NamedAlloc<T, Tag>>;

/// Un conjunto que dice que es.
template <typename K, typename Tag, typename Hash = std::hash<K>>
using NamedSet =
    std::unordered_set<K, Hash, std::equal_to<K>, NamedAlloc<K, Tag>>;

/// Un mapa que dice que es.
template <typename K, typename V, typename Tag, typename Hash = std::hash<K>>
using NamedMap = std::unordered_map<K, V, Hash, std::equal_to<K>,
                                    NamedAlloc<std::pair<const K, V>, Tag>>;

} // namespace util

#endif // VESTA_UTIL_NAMED_ALLOC_H
