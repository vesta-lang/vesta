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
 * @file request_router.h
 * @brief Enrutador de peticiones entre el servidor de sockets y el gestor de
 * VMs.
 *
 * RequestRouter es la capa intermedia del controlador de VestaVM.  Recibe los
 * mensajes brutos aceptados por el SocketServer, los parsea como comandos y los
 * redirige al VMManager que gestiona las instancias VM activas.
 *
 * Flujo del controlador:
 * @verbatim
 *   SocketServer  -> acepta conexiones y pasa los mensajes al router
 *        |
 *        v
 *   RequestRouter -> parsea comandos recibidos y los enruta al manager
 *        |
 *        v
 *   VMManager     -> gestiona las instancias VM segun el comando
 *        |
 *        v
 *   Runtime VM    -> ejecuta el bytecode en la instancia VM correcta
 * @endverbatim
 *
 * @note Clase pendiente de implementacion completa.
 */

#ifndef REQUEST_ROUTER_H
#define REQUEST_ROUTER_H

#endif // REQUEST_ROUTER_H
