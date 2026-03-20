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

#ifndef BYTECODE_H
#define BYTECODE_H

#include <cstdint>

namespace bytecode {

    enum class Opcodes : uint16_t {
        EDMW_IPv4   = 0x0000,
        EDMW_IPv6   = 0x0001,
        EDM         = 0x0002,
        HLT         = 0x0003,
    };

}

#endif
