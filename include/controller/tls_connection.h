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
 * @file tls_connection.h
 * @brief Conexion TCP cifrada con TLS sobre OpenSSL.
 *
 * TLSConnection extiende Connection para anadir cifrado TLS mediante OpenSSL.
 * Cuando se construye con un puntero SSL valido, las operaciones read_data()
 * y write_data() usan SSL_read() / SSL_write() en lugar de recv() / send().
 * Si el puntero SSL es nullptr la conexion opera sin cifrado (fallback).
 */

#ifndef TLS_CONNECTION_H
#define TLS_CONNECTION_H

#include "net/connection.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

/**
 * @brief Conexion TCP con soporte TLS opcional basado en OpenSSL.
 *
 * Hereda de Connection y sobreescribe start(), stop(), read_data() y
 * write_data() para operar sobre un canal SSL cuando tls_enabled es true. El
 * objeto SSL se libera en el destructor si fue proporcionado en el constructor.
 */
class TLSConnection : public Connection {
  private:
    SSL *ssl;         ///< Objeto SSL de OpenSSL; nullptr si TLS no esta activo
    bool tls_enabled; ///< true si la conexion opera sobre TLS

  public:
    /**
     * @brief Construye la conexion con el descriptor de socket y el objeto SSL.
     *
     * @param fd      Descriptor de socket del cliente ya aceptado.
     * @param ssl_ctx Puntero al objeto SSL (puede ser nullptr para conexion sin
     * cifrado).
     */
    TLSConnection(socket_t fd, SSL *ssl_ctx);

    /**
     * @brief Destructor: libera el objeto SSL si tls_enabled es true.
     */
    ~TLSConnection() override;

    /**
     * @brief Inicia la sesion de la conexion.
     *
     * Implementacion base: puede sobreescribirse en subclases para implementar
     * protocolos especificos (por ejemplo, enviar un saludo al cliente).
     */
    void start() override;

    /**
     * @brief Detiene la sesion cerrando el canal TLS y el socket.
     *
     * Si TLS esta habilitado realiza SSL_shutdown() antes de cerrar el socket.
     */
    void stop() override;

  protected:
    /**
     * @brief Devuelve el puntero al objeto SSL interno.
     *
     * Util para subclases que necesitan invocar operaciones SSL directamente
     * (p.ej. SSL_shutdown() en la implementacion de start()).
     *
     * @return Puntero al objeto SSL; nullptr si TLS no esta activo.
     */
    SSL *get_ssl() { return ssl; }

    /**
     * @brief Lee datos del canal (TLS o TCP segun configuracion).
     *
     * Usa SSL_read() si tls_enabled es true, o recv() en caso contrario.
     *
     * @param buffer Vector donde se almacenan los bytes leidos.
     * @return Numero de bytes leidos, 0 si la conexion cerro, o valor negativo
     * en error.
     */
    int read_data(std::vector<uint8_t> &buffer) override;

    /**
     * @brief Escribe datos en el canal (TLS o TCP segun configuracion).
     *
     * Usa SSL_write() si tls_enabled es true, o send() en caso contrario.
     *
     * @param buffer Vector con los bytes a enviar.
     * @return Numero de bytes escritos, o valor negativo en error.
     */
    int write_data(const std::vector<uint8_t> &buffer) override;
};

#endif // TLS_CONNECTION_H
