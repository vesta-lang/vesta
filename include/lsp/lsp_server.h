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
 * @file lsp_server.h
 * @brief Servidor Language Server Protocol para Vex (Fase 1).
 *
 * Despacha los metodos del LSP que esta fase soporta:
 *   - @c initialize / @c initialized / @c shutdown / @c exit
 *   - @c textDocument/didOpen / didChange / didClose (sincronizacion full)
 *
 * En @c didOpen y @c didChange compila el documento via @c AnalysisEngine y
 * publica @c textDocument/publishDiagnostics con los diagnosticos del
 * compilador mapeados a rangos LSP.  El motor de analisis se disena
 * reutilizable para que fases posteriores (semantic tokens, hover con
 * Big-O, ver IR/bytecode/JIT/AOT, diagramas) cuelguen del mismo punto.
 */

#ifndef VESTA_LSP_SERVER_H
#define VESTA_LSP_SERVER_H

#include "json.hpp"

#include "lsp/analysis_engine.h"
#include "lsp/document_store.h"
#include "lsp/json_rpc.h"

namespace lsp {

/**
 * @class LspServer
 * @brief Bucle principal + dispatcher de metodos LSP.
 *
 * Procesa mensajes de forma secuencial sobre un @c JsonRpcTransport.  No
 * es thread-safe por diseno (un solo hilo de eventos).
 */
class LspServer {
  public:
    /**
     * @brief Construye el servidor sobre el transporte dado.
     * @param transport Transporte JSON-RPC (por defecto stdio).
     */
    explicit LspServer(JsonRpcTransport transport);

    /**
     * @brief Ejecuta el bucle de eventos hasta recibir @c exit o EOF.
     * @return Codigo de salida del proceso (0 si fin limpio).
     */
    int run();

  private:
    /**
     * @brief Despacha un mensaje entrante segun su metodo.
     * @param msg Mensaje JSON-RPC ya parseado.
     */
    void dispatch(const nlohmann::json &msg);

    /// --- Handlers de peticiones (responden con result) ---
    void handle_initialize(const nlohmann::json &msg);
    void handle_shutdown(const nlohmann::json &msg);

    /// --- Handlers de notificaciones (no responden) ---
    void handle_did_open(const nlohmann::json &params);
    void handle_did_change(const nlohmann::json &params);
    void handle_did_close(const nlohmann::json &params);

    /**
     * @brief Compila el documento y publica sus diagnosticos al cliente.
     * @param uri URI del documento abierto/modificado.
     */
    void publish_diagnostics(const std::string &uri);

    /**
     * @brief Envia una respuesta de exito (result) a una peticion con id.
     * @param id     Identificador de la peticion (numero o string).
     * @param result Cuerpo del resultado.
     */
    void send_result(const nlohmann::json &id, const nlohmann::json &result);

    JsonRpcTransport transport_; ///< Transporte de mensajes.
    DocumentStore docs_;         ///< Documentos abiertos.
    AnalysisEngine engine_;      ///< Motor de analisis reutilizable.
    bool initialized_ = false;   ///< true tras un initialize correcto.
    bool shutdown_requested_ = false; ///< true tras shutdown (espera exit).
};

} // namespace lsp

#endif // VESTA_LSP_SERVER_H
