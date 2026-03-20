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

#include "controller/tls_connection.h"
#include <iostream>

TLSConnection::TLSConnection(socket_t fd, SSL *ssl_ctx)
    : Connection(fd), ssl(ssl_ctx), tls_enabled(true) {
    running.store(true);
}

TLSConnection::~TLSConnection() {
    stop();
}

void TLSConnection::start() {
    Connection::handle(); // usa el loop de la clase base
}

void TLSConnection::stop() {
    if (!running.load()) return;
    running.store(false);

    if (tls_enabled && ssl) {
        SSL_shutdown(ssl);  // cierra TLS correctamente
        SSL_free(ssl);
        ssl = nullptr;
    }

    close_socket(); // cierra el socket multiplataforma
}

// Lee hasta llenar 'buffer' o hasta error
int TLSConnection::read_data(std::vector<uint8_t> &buffer) {
    size_t total = 0;
    while (total < buffer.size()) {
        int n = SSL_read(ssl, buffer.data() + total, static_cast<int>(buffer.size() - total));
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue; // espera mas datos
            }
            return total > 0 ? static_cast<int>(total) : -1; // error o conexion cerrada
        }
        total += n;
    }
    return static_cast<int>(total);
}

// Escribe todos los bytes del buffer de manera segura
int TLSConnection::write_data(const std::vector<uint8_t> &buffer) {
    size_t total = 0;
    while (total < buffer.size()) {
        int n = SSL_write(ssl, buffer.data() + total, static_cast<int>(buffer.size() - total));
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue; // intenta de nuevo
            }
            return total > 0 ? static_cast<int>(total) : -1; // error grave
        }
        total += n;
    }
    return static_cast<int>(total);
}