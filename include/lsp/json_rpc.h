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
 * @file json_rpc.h
 * @brief Transporte JSON-RPC del Language Server Protocol sobre stdio.
 *
 * El LSP enmarca cada mensaje con una cabecera estilo HTTP:
 * @code
 *   Content-Length: <N>\r\n
 *   \r\n
 *   <cuerpo JSON de N bytes en UTF-8>
 * @endcode
 *
 * Esta clase lee y escribe esos mensajes en flujos binarios.  En Windows
 * el caller DEBE poner stdin/stdout en modo binario (ver
 * @c lsp::set_stdio_binary) para que los pares CRLF del framing no se
 * corrompan por la traduccion de saltos de linea del CRT.
 *
 * El parseo y la serializacion del cuerpo usan nlohmann/json, ya presente
 * en el proyecto.  Las lecturas son tolerantes: cabeceras desconocidas se
 * ignoran y solo @c Content-Length es obligatoria.
 */

#ifndef VESTA_LSP_JSON_RPC_H
#define VESTA_LSP_JSON_RPC_H

#include <cstdio>
#include <mutex>
#include <string>

#include "json.hpp"

namespace lsp {

/**
 * @brief Pone @c stdin y @c stdout en modo binario (no-op en POSIX).
 *
 * En Windows el CRT traduce LF<->CRLF por defecto, lo que romperia el
 * framing @c Content-Length del LSP.  Llamar una vez al arrancar el
 * servidor, antes de cualquier lectura/escritura de mensajes.
 */
void set_stdio_binary();

/**
 * @class JsonRpcTransport
 * @brief Lee y escribe mensajes LSP enmarcados por @c Content-Length.
 *
 * Trabaja directamente sobre @c FILE* (por defecto stdin/stdout) para
 * evitar el coste y los buffers intermedios de iostreams.  No es
 * thread-safe: el servidor procesa mensajes de forma secuencial.
 */
class JsonRpcTransport {
  public:
    /**
     * @brief Construye el transporte sobre los flujos dados.
     * @param in  Flujo de entrada (por defecto @c stdin).
     * @param out Flujo de salida (por defecto @c stdout).
     */
    explicit JsonRpcTransport(std::FILE *in = nullptr,
                              std::FILE *out = nullptr);

    /**
     * @brief Se mueve llevandose los flujos, con un cerrojo nuevo.
     *
     * Un cerrojo no se mueve -- ni tendria sentido: representa quien esta
     * escribiendo AHORA --.  Moverlo solo ocurre al montar el servidor, antes
     * de que exista ningun hilo y por tanto nadie a quien quitarle el turno.
     */
    JsonRpcTransport(JsonRpcTransport &&otro) noexcept;
    JsonRpcTransport &operator=(JsonRpcTransport &&otro) noexcept;
    JsonRpcTransport(const JsonRpcTransport &) = delete;
    JsonRpcTransport &operator=(const JsonRpcTransport &) = delete;

    /**
     * @brief Lee el siguiente mensaje completo del flujo de entrada.
     *
     * Bloquea hasta tener un mensaje entero o detectar EOF.  Parsea las
     * cabeceras hasta la linea en blanco, lee @c Content-Length bytes del
     * cuerpo y los deserializa como JSON.
     *
     * @param out_msg Recibe el objeto JSON parseado si el retorno es true.
     * @return true si se leyo un mensaje valido; false en EOF o error de
     *         framing/parse (el caller debe terminar el bucle).
     */
    bool read_message(nlohmann::json &out_msg);

    /**
     * @brief Serializa y escribe un mensaje con su cabecera de framing.
     *
     * Calcula @c Content-Length sobre los bytes UTF-8 del cuerpo, emite la
     * cabecera y el cuerpo, y hace flush para que el cliente lo reciba de
     * inmediato (los servidores LSP no deben bufferizar respuestas).
     *
     * @param msg Objeto JSON a enviar (request, response o notification).
     */
    void write_message(const nlohmann::json &msg);

  private:
    /**
     * @brief Lee una linea terminada en CRLF (o LF) del flujo de entrada.
     * @param out_line Recibe la linea SIN el terminador.
     * @return true si se leyo una linea; false en EOF antes de cualquier byte.
     */
    bool read_header_line(std::string &out_line);

    std::FILE *in_;  ///< Flujo de entrada (no se cierra; propiedad del caller).
    std::FILE *out_; ///< Flujo de salida (no se cierra; propiedad del caller).
    /// Un mensaje se escribe ENTERO o no se escribe: son varias llamadas
    /// (cabecera + cuerpo) y dos respuestas a la vez dejarian una dentro de la
    /// otra, con lo que el marco deja de cuadrar y la conversacion se pierde.
    mutable std::mutex escritura_;
};

} // namespace lsp

#endif // VESTA_LSP_JSON_RPC_H
