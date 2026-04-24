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
 * @file net.h
 * @brief Estructuras de direccion de red para comunicacion entre instancias VM.
 *
 * Define las estructuras HostIpv4 y HostIpv6 empaquetadas (sin padding) para
 * representar extremos de red con formato listo para transmision en la red
 * (network byte order).
 *
 * Uso tipico:
 * @code
 *   HostIpv4 h;
 *   h.ipv4 = htonl(0xC0A80001);  // 192.168.0.1
 *   h.port = htons(8080);
 *   // h puede enviarse como bloque de 6 bytes con memcpy o write_data()
 * @endcode
 */

#ifndef NET_H
#define NET_H

#include <cstdint>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
// htonl/htons disponibles a traves de winsock2.h incluido en connection.h
#else
#include <arpa/inet.h> // htonl/htons en sistemas POSIX
#endif

/**
 * @brief Extremo de red IPv4 compacto sin relleno de alineacion.
 *
 * Almacena la direccion IPv4 y el puerto en network byte order (big-endian)
 * listos para usarse directamente en operaciones de red.
 *
 * Ejemplo:
 * @code
 *   HostIpv4 h;
 *   h.ipv4 = htonl(0xC0A80001);  // 192.168.0.1 en network order
 *   h.port = htons(8080);        // puerto 8080 en network order
 * @endcode
 */
typedef struct __attribute__((packed)) HostIpv4 {
    uint32_t ipv4 = 0; ///< Direccion IPv4 en network byte order (big-endian)
    uint16_t port = 0; ///< Puerto en network byte order (big-endian)
} HostIpv4;

/**
 * @brief Extremo de red IPv6 compacto sin relleno de alineacion.
 *
 * Almacena los 16 bytes de la direccion IPv6 y el puerto en network byte order,
 * listos para usarse directamente en operaciones de red.
 */
typedef struct __attribute__((packed)) HostIpv6 {
    uint8_t  bytes[16] = {}; ///< Direccion IPv6 en network byte order (16 bytes)
    uint16_t port = 0;       ///< Puerto en network byte order (big-endian)
} HostIpv6;

#endif // NET_H
