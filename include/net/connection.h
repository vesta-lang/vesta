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

#ifndef CONNECTION_H
#define CONNECTION_H

#include <string>
#include <atomic>
#include <cstdint>
#include <vector>



#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
typedef SOCKET socket_t;
#else
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
typedef int socket_t;
#endif

#ifdef _WIN32
#include <winsock2.h>
#define SOCKET_INVALID INVALID_SOCKET
#else
#define SOCKET_INVALID -1
#endif


class Connection {
protected:
    socket_t          socket_fd;
    std::atomic<bool> running;

public:
    Connection(socket_t fd);

    virtual ~Connection();

    virtual void start();

    virtual void stop();

protected:
    virtual void handle();

    virtual int read_data(std::vector<uint8_t> &buffer);

    virtual int write_data(const std::vector<uint8_t> &buffer);

    virtual void process_request(const std::vector<uint8_t> &data);

    void close_socket();
};

#endif // CONNECTION_H
