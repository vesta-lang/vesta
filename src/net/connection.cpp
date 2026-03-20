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

#include "net/connection.h"
#include <iostream>

Connection::Connection(socket_t fd) : socket_fd(fd), running(true) {}

Connection::~Connection() {
    stop();
}

void Connection::start() {
    handle();
}

void Connection::stop() {
    if (!running.load()) return;
    running.store(false);
    close_socket();
}

void Connection::close_socket() {
#ifdef _WIN32
    if (socket_fd != INVALID_SOCKET) {
        closesocket(socket_fd);
        socket_fd = INVALID_SOCKET;
    }
#else
    if (socket_fd >= 0) {
        ::close(socket_fd);
        socket_fd = -1;
    }
#endif
}

void Connection::handle() {
    std::vector<uint8_t> buffer(4096);

    while (running.load()) {
        int n = read_data(buffer);
        if (n <= 0) break;

        buffer.resize(n); // recorta el buffer al tamaño real recibido
        process_request(buffer);
        buffer.resize(4096); // vuelve al tamaño inicial para la proxima lectura
    }

    stop();
}

int Connection::read_data(std::vector<uint8_t> &buffer) {
#ifdef _WIN32
    return recv(socket_fd, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
    return recv(socket_fd, reinterpret_cast<char*>(buffer.data()), buffer.size(), 0);
#endif
}

int Connection::write_data(const std::vector<uint8_t> &buffer) {
#ifdef _WIN32
    return send(socket_fd, reinterpret_cast<const char *>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
    return send(socket_fd, reinterpret_cast<const char*>(buffer.data()), buffer.size(), 0);
#endif
}

void Connection::process_request(const std::vector<uint8_t> &data) {
    std::string msg(data.begin(), data.end());
    std::cout << "[Connection] received: " << msg << std::endl;

    std::string          response = "OK\n";
    std::vector<uint8_t> resp_bytes(response.begin(), response.end());
    write_data(resp_bytes);
}
