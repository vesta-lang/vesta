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

#ifndef NET_H
#define NET_H

#include <cstdint>
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#else
#include <arpa/inet.h> // htonl/htons en Linux
#endif

/**
 * HostIpv4 h;
 * h.ipv4 = htonl(0xC0A80001);  // 192.168.0.1
 * h.port = htons(8080);
 * uint64_t r = h.raw;          // puede enviar como bloque de 8 bytes
 */
typedef struct __attribute__((packed)) HostIpv4 {
    uint32_t ipv4; // en network byte order
    uint16_t port; // en network byte order
} HostIpv4;


typedef struct __attribute__((packed)) HostIpv6 {
    uint8_t  bytes[16]; // dirección IPv6 (ya en network order)
    uint16_t port;      // puerto en network order
} HostIpv6;


#endif //NET_H
