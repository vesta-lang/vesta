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
 * @file tls_context.cpp
 * @brief Implementacion de @c TLSContext: contexto SSL/TLS del servidor.
 *
 * Crea el contexto OpenSSL (@c SSL_CTX) cargando el certificado PEM y la clave
 * privada.  Proporciona @c get() para obtener el puntero al contexto, y los
 * metodos estaticos @c initialize() / @c cleanup() para inicializar y liberar
 * la biblioteca OpenSSL globalmente.
 */
#include "controller/tls_context.h"
#include <iostream>

TLSContext::TLSContext(const std::string &cert_file,
                       const std::string &key_file) {
    ctx = TLS_server_method() ? SSL_CTX_new(TLS_server_method()) : nullptr;
    if (!ctx) {
        std::cerr << "Error creating SSL_CTX\n";
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_certificate_file(ctx, cert_file.c_str(),
                                     SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(), SSL_FILETYPE_PEM) <=
            0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
}

TLSContext::~TLSContext() {
    if (ctx) SSL_CTX_free(ctx);
}

void TLSContext::initialize() {
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
}

void TLSContext::cleanup() {
    EVP_cleanup();
}
