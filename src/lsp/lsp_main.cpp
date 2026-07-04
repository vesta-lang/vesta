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

/**
 * @brief Arranca el servidor LSP sobre stdio.
 * @return Codigo de salida del proceso.
 */
int main() {
    // Imprescindible en Windows: stdin/stdout en binario para no corromper
    // los pares CRLF del framing del LSP.
    lsp::set_stdio_binary();
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
