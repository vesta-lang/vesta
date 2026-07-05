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
 * @file tls_context.h
 * @brief Contexto TLS global basado en OpenSSL (SSL_CTX).
 *
 * TLSContext encapsula un SSL_CTX de OpenSSL inicializado con un certificado y
 * una clave privada.  Proporciona metodos estaticos para inicializar y limpiar
 * la biblioteca OpenSSL al inicio y al final del programa respectivamente.
 *
 * Uso tipico:
 * @code
 *   TLSContext::initialize();                         // una vez al inicio
 *   TLSContext ctx("server.crt", "server.key");      // cargar cert y clave
 *   // ... pasar ctx a TCPServer o ManagerTCPListener
 *   TLSContext::cleanup();                            // una vez al cierre
 * @endcode
 */

#ifndef TLS_CONTEXT_H
#define TLS_CONTEXT_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string>

/**
 * @brief Contexto TLS para servidor TLS basado en OpenSSL.
 *
 * Carga el certificado del servidor y la clave privada al construirse.
 * El objeto SSL_CTX interno se libera en el destructor.
 */
class TLSContext {
  private:
    SSL_CTX *ctx; ///< Contexto SSL de OpenSSL gestionado por este objeto

  public:
    /**
     * @brief Construye el contexto TLS cargando el certificado y la clave
     * privada indicados.
     *
     * Crea un SSL_CTX con TLS_server_method() y carga los ficheros mediante
     * SSL_CTX_use_certificate_file() y SSL_CTX_use_PrivateKey_file().
     * Lanza una excepcion std::runtime_error si algun paso falla.
     *
     * @param cert_file Ruta al fichero de certificado del servidor en formato
     * PEM.
     * @param key_file  Ruta al fichero de clave privada en formato PEM.
     */
    TLSContext(const std::string &cert_file, const std::string &key_file);

    /**
     * @brief Destructor: libera el SSL_CTX con SSL_CTX_free().
     */
    ~TLSContext();

    /**
     * @brief Devuelve el puntero al SSL_CTX interno.
     *
     * @return Puntero al contexto SSL de OpenSSL; nunca nullptr si el
     * constructor tuvo exito.
     */
    SSL_CTX *get() const { return ctx; }

    /**
     * @brief Inicializa la biblioteca OpenSSL.
     *
     * Debe llamarse una sola vez al inicio del programa, antes de crear
     * cualquier TLSContext o TLSConnection.
     * Invoca SSL_library_init(), SSL_load_error_strings() y
     * OpenSSL_add_all_algorithms().
     */
    static void initialize();

    /**
     * @brief Limpia los recursos globales de OpenSSL.
     *
     * Debe llamarse al final del programa para liberar la memoria de tablas
     * y estructuras internas de OpenSSL.
     */
    static void cleanup();
};

#endif // TLS_CONTEXT_H
