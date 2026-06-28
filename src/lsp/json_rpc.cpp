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
 * @file json_rpc.cpp
 * @brief Implementacion del transporte JSON-RPC del LSP sobre stdio.
 */

#include "lsp/json_rpc.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>

// En Windows hace falta poner los descriptores en modo binario para que el
// CRT no traduzca LF<->CRLF y corrompa el framing del LSP.
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace lsp {

void set_stdio_binary() {
#if defined(_WIN32)
    // _setmode evita la traduccion de saltos de linea en ambos flujos.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

JsonRpcTransport::JsonRpcTransport(std::FILE *in, std::FILE *out)
    : in_(in ? in : stdin), out_(out ? out : stdout) {}

bool JsonRpcTransport::read_header_line(std::string &out_line) {
    // Acumular bytes hasta encontrar el fin de linea.  Aceptamos CRLF (lo
    // normal en LSP) y tambien LF a secas por robustez frente a clientes
    // que envien lineas con solo LF.
    out_line.clear();
    int c;
    bool any = false; // se leyo al menos un byte antes del EOF.
    while ((c = std::fgetc(in_)) != EOF) {
        any = true;
        if (c == '\r') {
            // Posible CRLF: consumir el LF que deberia seguir.
            int next = std::fgetc(in_);
            if (next != '\n' && next != EOF) {
                // CR suelto: devolver el byte no esperado al flujo.
                std::ungetc(next, in_);
            }
            return true;
        }
        if (c == '\n') {
            // LF directo: fin de linea.
            return true;
        }
        // Byte normal de la cabecera.
        out_line.push_back(static_cast<char>(c));
    }
    // EOF: solo es "linea" si habiamos leido algo antes del corte.
    return any;
}

bool JsonRpcTransport::read_message(nlohmann::json &out_msg) {
    // 1) Leer cabeceras hasta la linea en blanco; capturar Content-Length.
    long content_length = -1;
    std::string line;
    bool saw_any_header = false;
    while (read_header_line(line)) {
        if (line.empty()) {
            // Linea en blanco: fin de cabeceras.
            break;
        }
        saw_any_header = true;
        // Buscar la cabecera Content-Length (case-insensitive en el nombre).
        const char *kKey = "content-length:";
        std::string lower;
        lower.reserve(line.size());
        for (char ch : line)
            lower.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(ch))));
        if (lower.rfind(kKey, 0) == 0) {
            // Extraer el numero tras los dos puntos, ignorando espacios.
            const char *p = line.c_str() + std::strlen(kKey);
            while (*p == ' ' || *p == '\t')
                ++p;
            content_length = std::strtol(p, nullptr, 10);
        }
        // Otras cabeceras (Content-Type, etc.) se ignoran a proposito.
    }

    // Si no leimos cabeceras ni cuerpo, asumimos EOF limpio.
    if (!saw_any_header && content_length < 0)
        return false;

    if (content_length < 0) {
        // Framing invalido: sin Content-Length no podemos leer el cuerpo.
        return false;
    }

    // 2) Leer exactamente content_length bytes del cuerpo.
    std::vector<char> body(static_cast<size_t>(content_length));
    size_t got = 0;
    while (got < body.size()) {
        size_t n = std::fread(body.data() + got, 1, body.size() - got, in_);
        if (n == 0)
            return false; // EOF prematuro: mensaje truncado.
        got += n;
    }

    // 3) Parsear el cuerpo como JSON.  Tolerante a errores: un cuerpo
    //    invalido no debe tumbar el servidor; el caller decide.
    out_msg = nlohmann::json::parse(body.begin(), body.end(),
                                    /*cb=*/nullptr,
                                    /*allow_exceptions=*/false);
    if (out_msg.is_discarded())
        return false;
    return true;
}

void JsonRpcTransport::write_message(const nlohmann::json &msg) {
    // Serializar el cuerpo sin indentacion (los clientes LSP no lo requieren
    // y ahorra bytes).  dump() devuelve UTF-8, por lo que la longitud en
    // bytes coincide con string::size().
    const std::string body = msg.dump();
    // Emitir la cabecera de framing seguida de la linea en blanco.
    std::fprintf(out_, "Content-Length: %zu\r\n\r\n", body.size());
    // Escribir el cuerpo exacto.
    std::fwrite(body.data(), 1, body.size(), out_);
    // Flush inmediato: el cliente espera la respuesta sin demoras de buffer.
    std::fflush(out_);
}

} // namespace lsp
