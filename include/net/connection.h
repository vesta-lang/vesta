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
 * @file connection.h
 * @brief Clase base abstracta para conexiones de red de VestaVM.
 *
 * Connection define la interfaz comun para cualquier tipo de conexion (TCP
 * plana, TLS, etc.).  Las subclases sobreescriben los metodos virtuales para
 * implementar el protocolo especifico de cada tipo de conexion.
 *
 * Tambien define el tipo socket_t de forma compatible con Windows (SOCKET) y
 * sistemas POSIX (int), y la constante SOCKET_INVALID para el valor centinela.
 */

#ifndef CONNECTION_H
#define CONNECTION_H

#include <string>
#include <atomic>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Portabilidad de sockets: Windows vs POSIX
// ---------------------------------------------------------------------------

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
typedef SOCKET socket_t; ///< Tipo de descriptor de socket en Windows
#else
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
typedef int socket_t; ///< Tipo de descriptor de socket en POSIX
#endif

#ifdef _WIN32
#include <winsock2.h>
#define SOCKET_INVALID                                                         \
    INVALID_SOCKET ///< Valor centinela de socket invalido (Windows)
#else
#define SOCKET_INVALID -1 ///< Valor centinela de socket invalido (POSIX)
#endif

/**
 * @brief Clase base abstracta para conexiones de red individuales.
 *
 * Encapsula un descriptor de socket y un indicador atomico de estado activo.
 * Las subclases deben sobreescribir start() para implementar la logica de
 * comunicacion y read_data() / write_data() para el transporte especifico.
 */
class Connection {
  protected:
    socket_t socket_fd;        ///< Descriptor de socket del cliente conectado
    std::atomic<bool> running; ///< true mientras la conexion esta activa

  public:
    /**
     * @brief Construye la conexion con el descriptor de socket indicado.
     *
     * Inicializa running a true de forma atomica.
     *
     * @param fd Descriptor de socket del cliente ya aceptado.
     */
    Connection(socket_t fd);

    /**
     * @brief Destructor virtual: llama a stop() si la conexion sigue activa.
     */
    virtual ~Connection();

    /**
     * @brief Inicia la sesion de la conexion.
     *
     * Implementacion por defecto: invoca handle() en un bucle mientras running
     * sea true. Las subclases pueden sobreescribir este metodo para implementar
     * protocolos especificos.
     */
    virtual void start();

    /**
     * @brief Detiene la conexion cerrando el socket y poniendo running a false.
     */
    virtual void stop();

  protected:
    /**
     * @brief Bucle de procesamiento: lee datos y los procesa hasta que la
     * conexion cierra.
     *
     * Implementacion por defecto: lee con read_data() y pasa los datos a
     * process_request(). Las subclases pueden sobreescribir este metodo para
     * control mas fino del bucle.
     */
    virtual void handle();

    /**
     * @brief Lee datos del socket en el vector @p buffer.
     *
     * Implementacion por defecto: usa recv() sobre el socket TCP sin cifrado.
     * Las subclases TLS sobreescriben este metodo para usar SSL_read().
     *
     * @param buffer Vector donde se almacenan los bytes recibidos.
     * @return Numero de bytes leidos; 0 si la conexion cerro; negativo en
     * error.
     */
    virtual int read_data(std::vector<uint8_t> &buffer);

    /**
     * @brief Escribe los datos del vector @p buffer en el socket.
     *
     * Implementacion por defecto: usa send() sobre el socket TCP sin cifrado.
     * Las subclases TLS sobreescriben este metodo para usar SSL_write().
     *
     * @param buffer Vector con los bytes a enviar.
     * @return Numero de bytes escritos; negativo en error.
     */
    virtual int write_data(const std::vector<uint8_t> &buffer);

    /**
     * @brief Procesa la peticion recibida.
     *
     * Implementacion por defecto: no hace nada.  Las subclases sobreescriben
     * este metodo para implementar la logica de protocolo.
     *
     * @param data Bytes recibidos listos para procesar.
     */
    virtual void process_request(const std::vector<uint8_t> &data);

    /**
     * @brief Cierra el descriptor de socket de forma compatible con la
     * plataforma.
     *
     * Usa closesocket() en Windows y close() en POSIX.
     */
    void close_socket();
};

#endif // CONNECTION_H
