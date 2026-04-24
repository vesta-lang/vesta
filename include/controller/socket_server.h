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
 * @file socket_server.h
 * @brief Marcador de inclusion para el modulo de servidor de sockets (pendiente de implementacion).
 *
 * Este fichero reserva el espacio para la futura clase SocketServer que actuara como
 * capa de abstraccion del servidor TCP/TLS antes del enrutamiento de peticiones.
 *
 * Arquitectura del controlador:
 * @verbatim
 *   SocketServer  -> acepta conexiones entrantes y pasa los mensajes
 *        |
 *        v
 *   RequestRouter -> parsea comandos recibidos y los enruta
 *        |
 *        v
 *   VMManager     -> gestiona las instancias VM segun el comando
 *        |
 *        v
 *   Runtime VM    -> ejecuta el bytecode en la instancia VM correcta
 * @endverbatim
 */

#ifndef SOCKET_SERVER_H
#define SOCKET_SERVER_H

#endif // SOCKET_SERVER_H
