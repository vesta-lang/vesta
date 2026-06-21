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
 * @file lsp_server.cpp
 * @brief Implementacion del dispatcher y bucle de eventos del LSP de Vex.
 */

#include "lsp/lsp_server.h"

#include <exception>
#include <string>
#include <utility>

#include "lsp/semantic_tokens.h"
#include "vex/diagnostic.h"

namespace lsp {

namespace {

/**
 * @brief Mapea la severidad del compilador a la del LSP.
 *
 * LSP: 1=Error, 2=Warning, 3=Information, 4=Hint.  Las NOTE del
 * compilador se mapean a Information (3).
 *
 * @param level Nivel del diagnostico Vex.
 * @return Codigo de severidad LSP.
 */
int diag_severity_to_lsp(vex::DiagLevel level) {
    switch (level) {
    case vex::DiagLevel::ERR: return 1;  // Error
    case vex::DiagLevel::WARN: return 2; // Warning
    case vex::DiagLevel::NOTE: return 3; // Information
    }
    return 1;
}

} // namespace

LspServer::LspServer(JsonRpcTransport transport)
    : transport_(std::move(transport)) {}

void LspServer::send_result(const nlohmann::json &id,
                            const nlohmann::json &result) {
    // Respuesta JSON-RPC 2.0 estandar: jsonrpc + id + result.
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = result;
    transport_.write_message(resp);
}

void LspServer::handle_initialize(const nlohmann::json &msg) {
    // Anunciar las capacidades soportadas en esta fase.  textDocumentSync=1
    // = sincronizacion full (cada cambio envia el texto completo).  Se deja
    // sitio para anunciar capacidades futuras (semanticTokens, hover, etc.).
    nlohmann::json caps;
    caps["textDocumentSync"] = 1; // TextDocumentSyncKind.Full

    // Resaltado semantico (Fase 2): anunciar la leyenda (tokenTypes +
    // tokenModifiers, en orden = indices) y soporte de documento completo.
    // No se anuncia range/delta en esta fase.
    nlohmann::json sem;
    nlohmann::json legend;
    legend["tokenTypes"] = semantic_token_types();
    legend["tokenModifiers"] = semantic_token_modifiers();
    sem["legend"] = std::move(legend);
    sem["full"] = true;
    sem["range"] = false;
    caps["semanticTokensProvider"] = std::move(sem);

    // Inspector del ecosistema (Fase 3): las peticiones a medida vesta/* no
    // forman parte del LSP estandar, asi que se anuncian bajo el campo
    // experimental para que un cliente que las conozca las descubra.
    nlohmann::json experimental;
    experimental["vestaMethods"] = nlohmann::json::array(
        {"vesta/bytecode", "vesta/ir", "vesta/complexity", "vesta/diagram",
         "vesta/functions", "vesta/aotCompat", "vesta/jitAsm", "vesta/aotAsm"});
    caps["experimental"] = std::move(experimental);

    nlohmann::json result;
    result["capabilities"] = caps;
    nlohmann::json server_info;
    server_info["name"] = "vesta-lsp";
    server_info["version"] = "0.1.0";
    result["serverInfo"] = server_info;

    // Responder a la peticion (initialize SIEMPRE lleva id).
    send_result(msg.at("id"), result);
    initialized_ = true;
}

void LspServer::handle_shutdown(const nlohmann::json &msg) {
    // El cliente pide cerrar: responder result null y esperar el exit.
    shutdown_requested_ = true;
    send_result(msg.at("id"), nullptr);
}

void LspServer::handle_did_open(const nlohmann::json &params) {
    // params.textDocument = { uri, languageId, version, text }
    const auto &td = params.at("textDocument");
    const std::string uri = td.at("uri").get<std::string>();
    std::string text = td.value("text", std::string());
    docs_.open(uri, text);
    publish_diagnostics(uri);
}

void LspServer::handle_did_change(const nlohmann::json &params) {
    // Sincronizacion full: tomamos el texto del ultimo contentChange.
    const auto &td = params.at("textDocument");
    const std::string uri = td.at("uri").get<std::string>();
    if (!params.contains("contentChanges"))
        return;
    const auto &changes = params.at("contentChanges");
    if (!changes.is_array() || changes.empty())
        return;
    // En modo full sync, el ultimo cambio contiene el documento entero.
    const auto &last = changes.back();
    std::string text = last.value("text", std::string());
    docs_.update(uri, text);
    publish_diagnostics(uri);
}

void LspServer::handle_did_close(const nlohmann::json &params) {
    const auto &td = params.at("textDocument");
    const std::string uri = td.at("uri").get<std::string>();
    docs_.close(uri);
    engine_.forget(uri);
    // Publicar una lista de diagnosticos vacia limpia los del editor.
    nlohmann::json note;
    note["jsonrpc"] = "2.0";
    note["method"] = "textDocument/publishDiagnostics";
    nlohmann::json p;
    p["uri"] = uri;
    p["diagnostics"] = nlohmann::json::array();
    note["params"] = p;
    transport_.write_message(note);
}

void LspServer::publish_diagnostics(const std::string &uri) {
    // Compilar (o reusar cache) el documento.
    const std::string &text = docs_.text(uri);
    const DocAnalysis &an = engine_.analyze_document(uri, text);

    // Construir el array de diagnosticos LSP a partir de los del compilador.
    nlohmann::json diags = nlohmann::json::array();
    for (const auto &d : an.result.diagnostics.all()) {
        // El compilador da linea/columna 1-based en bytes; el LSP exige
        // 0-based + caracter en UTF-16.  Convertir cada extremo del span.
        const vex::SourceLoc &loc = d.loc;
        // Linea 0-based (proteger contra line==0 hipotetico).
        uint32_t line0 = loc.line > 0 ? loc.line - 1 : 0;
        // Texto de la linea para la conversion de columnas.
        std::string line_text = docs_.line(uri, line0);
        // Caracter inicial en UTF-16.
        uint32_t start_char = byte_column_to_utf16(line_text, loc.column);
        // Caracter final: columna inicial + longitud del span (en bytes).
        // La columna 1-based del extremo final es column + length.
        uint32_t end_col_1based = loc.column + (loc.length > 0 ? loc.length : 1);
        uint32_t end_char = byte_column_to_utf16(line_text, end_col_1based);

        nlohmann::json range;
        range["start"] = {{"line", line0}, {"character", start_char}};
        // El span no cruza lineas en el modelo actual del compilador.
        range["end"] = {{"line", line0}, {"character", end_char}};

        nlohmann::json jd;
        jd["range"] = range;
        jd["severity"] = diag_severity_to_lsp(d.level);
        jd["source"] = "vesta";
        if (!d.code.empty())
            jd["code"] = d.code;
        jd["message"] = d.message;
        diags.push_back(std::move(jd));
    }

    // Emitir la notificacion publishDiagnostics.
    nlohmann::json note;
    note["jsonrpc"] = "2.0";
    note["method"] = "textDocument/publishDiagnostics";
    nlohmann::json p;
    p["uri"] = uri;
    p["diagnostics"] = std::move(diags);
    note["params"] = std::move(p);
    transport_.write_message(note);
}

void LspServer::handle_semantic_tokens_full(const nlohmann::json &msg) {
    // params.textDocument.uri identifica el documento.
    const auto &params = msg.at("params");
    const auto &td = params.at("textDocument");
    const std::string uri = td.at("uri").get<std::string>();

    // Calcular los tokens.  Si el documento no esta abierto, devolvemos una
    // lista vacia (data: []) en lugar de un error: el cliente lo tolera.
    nlohmann::json data = nlohmann::json::array();
    if (docs_.has(uri)) {
        const std::string &text = docs_.text(uri);
        // Reusar el analisis cacheado (mismo punto que los diagnosticos) para
        // enriquecer los identificadores con los nombres declarados.
        const DocAnalysis &an = engine_.analyze_document(uri, text);
        std::vector<uint32_t> toks =
            compute_semantic_tokens(text, uri, &an);
        // Volcar el array plano de uint32 a JSON.
        data = nlohmann::json(toks);
    }

    nlohmann::json result;
    result["data"] = std::move(data);
    send_result(msg.at("id"), result);
}

bool LspServer::handle_vesta_request(const std::string &method,
                                     const nlohmann::json &msg) {
    // Toda peticion vesta/* lleva id (es request) y params con el uri.  Si
    // falta algo, respondemos con un error en lugar de crashear.
    if (!msg.contains("id"))
        return false; // sin id no es una peticion: dejar pasar.
    const nlohmann::json &id = msg.at("id");

    // Helper local para responder un error con la forma { error: "..." }.
    auto respond_error = [&](const std::string &what) {
        nlohmann::json r;
        r["error"] = what;
        send_result(id, r);
    };

    try {
        // Extraer params.uri (comun a todas las peticiones del inspector).
        if (!msg.contains("params") || !msg.at("params").is_object()) {
            respond_error("faltan params");
            return true;
        }
        const nlohmann::json &params = msg.at("params");
        const std::string uri = params.value("uri", std::string());
        if (uri.empty()) {
            respond_error("falta params.uri");
            return true;
        }

        nlohmann::json result;
        if (method == "vesta/bytecode") {
            result = inspector_.bytecode(uri);
        } else if (method == "vesta/ir") {
            const std::string phase = params.value("phase", std::string("post"));
            result = inspector_.ir(uri, phase);
        } else if (method == "vesta/complexity") {
            result = inspector_.complexity(uri);
        } else if (method == "vesta/diagram") {
            const std::string kind = params.value("kind", std::string("ir-post"));
            const std::string format =
                params.value("format", std::string("mermaid"));
            const bool cost = params.value("cost", false);
            result = inspector_.diagram(uri, kind, format, cost);
        } else if (method == "vesta/functions") {
            result = inspector_.functions(uri);
        } else if (method == "vesta/aotCompat") {
            const std::string tier = params.value("tier", std::string("bare"));
            result = inspector_.aot_compat(uri, tier);
        } else if (method == "vesta/jitAsm") {
            const std::string fn = params.value("function", std::string());
            result = inspector_.jit_asm(uri, fn);
        } else if (method == "vesta/aotAsm") {
            const std::string fn = params.value("function", std::string());
            result = inspector_.aot_asm(uri, fn);
        } else {
            // Metodo vesta/* desconocido: no manejado aqui.
            return false;
        }
        send_result(id, result);
        return true;
    } catch (const std::exception &e) {
        // Cualquier fallo del inspector se convierte en un error de
        // resultado: el servidor sigue sirviendo.
        respond_error(std::string("excepcion en el inspector: ") + e.what());
        return true;
    } catch (...) {
        respond_error("excepcion desconocida en el inspector");
        return true;
    }
}

void LspServer::dispatch(const nlohmann::json &msg) {
    // Todo mensaje LSP valido lleva un campo method (string).  Sin el, lo
    // ignoramos (puede ser una respuesta a una peticion nuestra, etc.).
    if (!msg.contains("method") || !msg.at("method").is_string())
        return;
    const std::string method = msg.at("method").get<std::string>();

    // Peticiones (llevan id) y notificaciones (no llevan id) se distinguen
    // por la presencia del campo id.
    if (method == "initialize") {
        handle_initialize(msg);
    } else if (method == "initialized") {
        // Notificacion sin payload relevante: no requiere respuesta.
    } else if (method == "shutdown") {
        handle_shutdown(msg);
    } else if (method == "exit") {
        // exit se maneja en el bucle run(); aqui no hacemos nada.
    } else if (method == "textDocument/didOpen") {
        handle_did_open(msg.at("params"));
    } else if (method == "textDocument/didChange") {
        handle_did_change(msg.at("params"));
    } else if (method == "textDocument/didClose") {
        handle_did_close(msg.at("params"));
    } else if (method == "textDocument/semanticTokens/full") {
        handle_semantic_tokens_full(msg);
    } else if (method.rfind("vesta/", 0) == 0 &&
               handle_vesta_request(method, msg)) {
        // Peticion a medida del inspector (vesta/*) atendida.
    } else if (msg.contains("id")) {
        // Peticion de un metodo no soportado: responder error MethodNotFound
        // para cumplir el protocolo (las peticiones SIEMPRE deben responderse).
        nlohmann::json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = msg.at("id");
        resp["error"] = {{"code", -32601}, // MethodNotFound
                         {"message", "metodo no soportado: " + method}};
        transport_.write_message(resp);
    }
    // Notificaciones no soportadas: se ignoran en silencio (permitido).
}

int LspServer::run() {
    nlohmann::json msg;
    // Bucle de eventos: leer-despachar hasta EOF o exit.
    while (transport_.read_message(msg)) {
        // Detectar exit antes de despachar para terminar limpio.
        if (msg.contains("method") && msg.at("method").is_string() &&
            msg.at("method").get<std::string>() == "exit") {
            // Por protocolo: exit tras shutdown => codigo 0; sin shutdown => 1.
            return shutdown_requested_ ? 0 : 1;
        }
        // Despachar bajo try/catch: un mensaje malformado (campos ausentes)
        // no debe tumbar el servidor.
        try {
            dispatch(msg);
        } catch (...) {
            // Ignorar errores de un mensaje individual y seguir sirviendo.
        }
    }
    // EOF sin exit explicito: salida limpia.
    return 0;
}

} // namespace lsp
