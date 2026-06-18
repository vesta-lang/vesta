/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file vm_address_space.h
 * @brief Constantes de los espacios de direcciones virtuales de la VM.
 *
 * Define los rangos de direcciones virtuales de 64 bits reservados para
 * cada zona del espacio de memoria de un proceso VestaVM.  Estos valores
 * son los defaults; cada instancia puede sobreescribirlos en tiempo de
 * ejecucion si se necesita un mapeado distinto.
 *
 * Layout del espacio de direcciones (resumen):
 *
 *   [0x0000_0000_0000_0000 - 0x0000_0000_FFFF_FFFF]  4 GiB   zona libre
 *   [0x0000_0001_0000_0000 - 0x0000_0FFF_FFFF_FFFF]  ~4 TiB  pila
 *   [0x0000_1000_0000_0000 - 0x0000_1FFF_FFFF_FFFF]  1 TiB   codigo
 *   [0x0000_2000_0000_0000 - 0x0000_2FFF_FFFF_FFFF]  1 TiB   metadatos de
 * codigo [0x0000_3000_0000_0000 - 0x0000_3FFF_FFFF_FFFF]  1 TiB   codigo JIT
 *   [0x1000_0000_0000_0000 - 0x1FFF_FFFF_FFFF_FFFF]  1/16 VA mapeo de host
 *   [0x2000_0000_0000_0000 - 0x2FFF_FFFF_FFFF_FFFF]  1/16 VA mapeo de manager
 *   [0x3000_0000_0000_0000 - 0x3FFF_FFFF_FFFF_FFFF]  1/16 VA mapeo remoto
 */

#ifndef VM_ADDRESS_SPACE_H
#define VM_ADDRESS_SPACE_H

#include <cstdint>

namespace vm::mem {

// --- Zona libre (primeros 4 GiB del espacio de direcciones) ---
constexpr std::uint64_t FREE_BEGIN =
    0x0000000000000000ull; ///< Inicio de la zona de uso libre
constexpr std::uint64_t FREE_END =
    0x00000000FFFFFFFFull; ///< Fin de la zona de uso libre (4 GiB)

// --- Zona de pila (hasta ~4 TiB) ---
constexpr std::uint64_t STACK_BEGIN =
    0x0000000100000000ull; ///< Inicio del segmento de pila
constexpr std::uint64_t STACK_END =
    0x00000FFFFFFFFFFFull; ///< Fin del segmento de pila (~4 TiB max)

// --- Zona de codigo (1 TiB) ---
constexpr std::uint64_t CODE_BEGIN =
    0x0000100000000000ull; ///< Inicio del segmento de codigo (.velb)
constexpr std::uint64_t CODE_END =
    0x00001FFFFFFFFFFFull; ///< Fin del segmento de codigo (1 TiB max)

// --- Zona de metadatos de codigo (1 TiB) ---
constexpr std::uint64_t META_BEGIN =
    0x0000200000000000ull; ///< Inicio de la zona de metadatos de codigo
constexpr std::uint64_t META_END =
    0x00002FFFFFFFFFFFull; ///< Fin de la zona de metadatos (1 TiB max)

// --- Zona de codigo JIT (1 TiB) ---
constexpr std::uint64_t JIT_BEGIN =
    0x0000300000000000ull; ///< Inicio de la zona de codigo generado por JIT
constexpr std::uint64_t JIT_END =
    0x00003FFFFFFFFFFFull; ///< Fin de la zona JIT (1 TiB max)

// --- Zona de mapeo de direcciones del host (locales o virtuales) ---
constexpr std::uint64_t HOSTMAP_BEGIN =
    0x1000000000000000ull; ///< Inicio del mapeo de punteros de host reales o
                           ///< virtuales
constexpr std::uint64_t HOSTMAP_END =
    0x1FFFFFFFFFFFFFFFull; ///< Fin del mapeo de host (1/16 del VA total)

// --- Zona de mapeo del gestor de instancias (ptrManager -> ptrVirtualLocal)
// ---
constexpr std::uint64_t MGRMAP_BEGIN =
    0x2000000000000000ull; ///< Inicio del mapeo de punteros del manager a
                           ///< punteros virtuales locales
constexpr std::uint64_t MGRMAP_END =
    0x2FFFFFFFFFFFFFFFull; ///< Fin del mapeo de manager (1/16 del VA total)

// --- Zona de mapeo remoto (ptrVirtualHostRemoto -> ptrVirtualLocal) ---
constexpr std::uint64_t REMOTEMAP_BEGIN =
    0x3000000000000000ull; ///< Inicio del mapeo de punteros remotos a punteros
                           ///< virtuales locales
constexpr std::uint64_t REMOTEMAP_END =
    0x3FFFFFFFFFFFFFFFull; ///< Fin del mapeo remoto (1/16 del VA total)

} // namespace vm::mem

#endif // VM_ADDRESS_SPACE_H
