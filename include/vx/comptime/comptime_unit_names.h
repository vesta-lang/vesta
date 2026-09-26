/**
 * @file comptime_unit_names.h
 * @brief Los nombres de las funciones que forman el conjunto comptime de un
 *        modulo, INTERNADOS.
 *
 * Los usan tres duenos -- lo que se compila de un modulo, su `.vxi` y el
 * resultado de compilar -- y los consume el filtro que se queda, del modulo
 * fusionado, solo con lo que la maquina de compilacion tiene que poder
 * ejecutar.  Internados, ese filtro compara por IDENTIDAD con
 * `IrFunction::name_key()` en vez de hashear cadenas.
 */
#ifndef VX_COMPTIME_COMPTIME_UNIT_NAMES_H
#define VX_COMPTIME_COMPTIME_UNIT_NAMES_H

#include "util/name_pool.h"   // util::InternedName
#include "util/named_alloc.h" // util::NamedVector

namespace vx {

namespace scratch {
struct ComptimeUnitNames; ///< Nombres del conjunto comptime de un modulo.
} // namespace scratch

/// Los nombres del conjunto comptime de un modulo, en el orden en que se
/// recogieron.
using ComptimeUnitNames =
    util::NamedVector<util::InternedName, scratch::ComptimeUnitNames>;

} // namespace vx

#endif // VX_COMPTIME_COMPTIME_UNIT_NAMES_H
