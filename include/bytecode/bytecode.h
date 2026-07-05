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
 * @file bytecode.h
 * @brief Definicion de los opcodes de control del protocolo de bytecode
 * VestaVM.
 *
 * Contiene el enum Opcodes que enumera los codigos de operacion de nivel de
 * protocolo usados para comunicacion entre instancias VM distribuidas (EDM) y
 * control de ejecucion (HLT).
 */

#ifndef BYTECODE_H
#define BYTECODE_H

#include <cstdint>

namespace bytecode {

/**
 * @brief Codigos de operacion de protocolo de la maquina virtual.
 *
 * Estos opcodes se usan en el protocolo de comunicacion entre instancias VM
 * y en el flujo de control del runtime distribuido:
 *
 *   - EDMW_IPv4: mensaje de distribucion de trabajo con destino IPv4.
 *   - EDMW_IPv6: mensaje de distribucion de trabajo con destino IPv6.
 *   - EDM:       mensaje de distribucion de trabajo generico (sin direccion de
 * red explicita).
 *   - HLT:       detener la ejecucion de la instancia VM actual.
 */
enum class Opcodes : uint16_t {
    EDMW_IPv4 = 0x0000, ///< Distribuir trabajo a una VM remota via IPv4
    EDMW_IPv6 = 0x0001, ///< Distribuir trabajo a una VM remota via IPv6
    EDM =
        0x0002, ///< Distribuir trabajo (direccion de red resuelta externamente)
    HLT = 0x0003, ///< Detener la ejecucion de la VM
};

} // namespace bytecode

#endif // BYTECODE_H
