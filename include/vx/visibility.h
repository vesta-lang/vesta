/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file visibility.h
 * @brief Quien puede ver un miembro (o una declaracion).
 *
 * Vive aparte del arbol porque la usan tres capas: el arbol (lo escrito), el
 * comprobador (las fichas de los miembros) y la interfaz de modulo (`.vxi`),
 * que la lleva de un modulo a otro.  Un solo tipo para las tres: si cada una
 * tuviera el suyo, habria que traducir -- y una traduccion olvidada deja un
 * `private` convertido en otra cosa.
 */

#ifndef VX_VISIBILITY_H
#define VX_VISIBILITY_H

#include <cstdint>

namespace vx {

/**
 * @enum Visibility
 * @brief Quien puede ver un miembro (o una declaracion).
 *
 * UN solo sistema para todo lo que se declara: campos y metodos de struct,
 * overlay, clase, `impl` y concepto.  De menos a mas:
 *
 * | nivel       | quien lo ve                          |
 * | :---------- | :----------------------------------- |
 * | `private`   | solo el tipo que lo escribe          |
 * | `protected` | el tipo y sus derivados              |
 * | sin palabra | el propio modulo                     |
 * | `internal`  | los modulos del mismo paquete        |
 * | `public`    | todos                                |
 *
 * Un `impl` sobre un tipo es codigo de FUERA del tipo: no ve sus privados.
 *
 * Los valores viajan en el `.vxi`: no se reordenan sin subir su version.
 */
enum class Visibility : uint8_t {
    Unwritten = 0, ///< sin palabra: el propio modulo
    Public = 1,    ///< `public`
    Internal = 2,  ///< `internal`: el paquete
    Protected = 3, ///< `protected`: el tipo y sus derivados
    Private = 4,   ///< `private`: solo el tipo
};

/// El mayor valor valido de @ref Visibility (para validar lo que se lee).
inline constexpr uint8_t kVisibilityMax =
    static_cast<uint8_t>(Visibility::Private);

} // namespace vx

#endif // VX_VISIBILITY_H
