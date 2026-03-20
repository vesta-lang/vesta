/*
 * VestaVM - Máquina Virtual Distribuida
 * 
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 * 
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 * 
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * Este archivo contiene los espacios de direcciones por defecto para todas las instancias y manager
 * de la VM, aunque estos se pueden cambiar en la ejecuccion de la instancia.
 */
#ifndef VM_ADDRESS_SPACE_H
#define VM_ADDRESS_SPACE_H

#include <cstdint>

namespace vm::mem {
    // <- Uso "libre"   4GB
    constexpr std::uint64_t FREE_BEGIN      = 0x0000000000000000ull;
    constexpr std::uint64_t FREE_END        = 0x00000000FFFFFFFFull;
    // <- Uso "libre"

    // <- Uso "stack"                   1Tb max
    constexpr std::uint64_t STACK_BEGIN     = 0x0000000100000000ull;
    constexpr std::uint64_t STACK_END       = 0x00000FFFFFFFFFFFull;
    // <- Uso "stack"

    // <- Uso "codigo"                  1Tb max
    constexpr std::uint64_t CODE_BEGIN      = 0x0000100000000000ull;
    constexpr std::uint64_t CODE_END        = 0x00001FFFFFFFFFFFull;
    // <- Uso "codigo"

    // <- Uso "metadatos de codigo"     1Tb max
    constexpr std::uint64_t META_BEGIN      = 0x0000200000000000ull;
    constexpr std::uint64_t META_END        = 0x00002FFFFFFFFFFFull;
    // <- Uso "metadatos de codigo"

    // <- Uso "zona de codigo JIT"      1Tb max
    constexpr std::uint64_t JIT_BEGIN       = 0x0000300000000000ull;
    constexpr std::uint64_t JIT_END         = 0x00003FFFFFFFFFFFull;
    // <- Uso "zona de codigo JIT"

    // <- Uso "mapeo de direcciones Host reales o virtuales"
    constexpr std::uint64_t HOSTMAP_BEGIN   = 0x1000000000000000ull;
    constexpr std::uint64_t HOSTMAP_END     = 0x1FFFFFFFFFFFFFFFull;
    // <- Uso "mapeo de direcciones Host reales o virtuales"

    // <- ptrManager a ptrVirtualLocal
    constexpr std::uint64_t MGRMAP_BEGIN    = 0x2000000000000000ull;
    constexpr std::uint64_t MGRMAP_END      = 0x2FFFFFFFFFFFFFFFull;
    // <- ptrManager a ptrVirtualLocal

    // <- ptrVirtualHostRemoto a ptrVirtualLocal
    constexpr std::uint64_t REMOTEMAP_BEGIN = 0x3000000000000000ull;
    constexpr std::uint64_t REMOTEMAP_END   = 0x3FFFFFFFFFFFFFFFull;
    // <- ptrVirtualHostRemoto a ptrVirtualLocal
}

#endif //VM_ADDRESS_SPACE_H
