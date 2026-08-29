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
 * @file lsp_main.cpp
 * @brief Punto de entrada del servidor LSP de Vesta (binario @c vesta_lsp).
 *
 * Habla Language Server Protocol por stdio.  El editor lanza este proceso
 * y se comunica con el via stdin/stdout (JSON-RPC enmarcado por
 * @c Content-Length).  No toma argumentos en esta fase.
 */

#include "jit/keystone_asm_backend.h"
#include "lsp/json_rpc.h"
#include "lsp/lsp_server.h"
#include "vx/diag/diag_catalog.h"

/**
 * @brief Arranca el servidor LSP sobre stdio.
 * @return Codigo de salida del proceso.
 */
int main() {
    // Imprescindible en Windows: stdin/stdout en binario para no corromper
    // los pares CRLF del framing del LSP.
    lsp::set_stdio_binary();
    // El idioma de todo lo que el servidor manda al editor -- diagnosticos y
    // texto de las vistas -- sale del entorno (VESTA_LANG, LC_ALL, LANG), igual
    // que en la linea de ordenes.  Sin esta linea el servidor se quedaba
    // siempre en el idioma cero, y el catalogo no servia de nada por su lado.
    vx::diag::set_language(vx::diag::language_from_env());
    // Instalar el backend de ensamblado (Keystone) para que el analisis valide
    // el inline asm (@Asm / asm{}) y reporte instrucciones invalidas como
    // diagnosticos con linea/columna -- sin esto, lower_asm omite la validacion
    // y el asm roto pasa silencioso.
    jit::register_keystone_asm_backend();
    // Transporte por defecto = stdin/stdout.
    lsp::JsonRpcTransport transport;
    lsp::LspServer server(std::move(transport));
    return server.run();
}
