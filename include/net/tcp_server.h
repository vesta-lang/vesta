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

#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "controller/tls_connection.h"
#include "controller/tls_context.h"
#include "net/connection.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
typedef SOCKET socket_t;
#define INVALID_SOCKET_VALUE INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
typedef int socket_t;
#define INVALID_SOCKET_VALUE -1
#endif


inline void close_socket(socket_t s) {
#ifdef _WIN32
    if (s != INVALID_SOCKET) closesocket(s);
#else
    if (s >= 0) close(s);
#endif
}


class TCPServer {
protected:
    socket_t server_fd{INVALID_SOCKET_VALUE};
    uint16_t          port;
    std::atomic<bool> running{false};

    bool        tls_enabled{false};
    TLSContext *tls_ctx{nullptr};

    std::vector<std::thread> threads;

public:
    TCPServer(uint16_t port);

    TCPServer(uint16_t port, TLSContext *ctx);

    virtual ~TCPServer();

    bool start();

    void stop();

protected:
    // Metodo que se puede sobrescribir para crear la conexión del cliente
    virtual Connection *create_connection(socket_t client_fd, TLSContext *tls_ctx) {
        if (tls_enabled && tls_ctx) {
            SSL *ssl = SSL_new(tls_ctx->get());
            SSL_set_fd(ssl, (int) client_fd);
            if (SSL_accept(ssl) <= 0) {
                ERR_print_errors_fp(stderr);
                SSL_free(ssl);
                return nullptr;
            }
            return new TLSConnection(client_fd, ssl);
        } else {
            return new Connection(client_fd);
        }
    }

    virtual void accept_loop();
};

#endif // TCP_SERVER_H
