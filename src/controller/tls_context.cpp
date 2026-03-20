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

#include "controller/tls_context.h"
#include <iostream>

TLSContext::TLSContext(const std::string &cert_file, const std::string &key_file) {
    ctx = TLS_server_method() ? SSL_CTX_new(TLS_server_method()) : nullptr;
    if (!ctx) {
        std::cerr << "Error creating SSL_CTX\n";
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_certificate_file(ctx, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
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
