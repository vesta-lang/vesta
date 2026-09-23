/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file lsp/queries.h
 * @brief Las CONSULTAS que contesta el inspector, como filas de la tabla
 *        compartida.
 *
 * La tabla (@c query::QueryDesc) es el mecanismo y no sabe que existe el
 * inspector; aqui estan las filas, que es lo unico que conoce a los dos lados.
 * Por eso viven en `src/lsp/` y no en `src/query/`: si la capa compartida
 * dependiera de uno de sus consumidores dejaria de ser compartida.
 */
#ifndef LSP_QUERIES_H
#define LSP_QUERIES_H

#include "lsp/inspector.h"
#include "query/query_table.h"

namespace lsp {

class DocumentStore;

/**
 * @brief Lo que sabe hacer el SERVIDOR y no el inspector.
 *
 * Dos consultas -- compilar a disco y el simbolo bajo el cursor -- se contestan
 * con estado que vive en el servidor, no en el inspector.  Podrian haberse
 * quedado fuera de la tabla, despachadas a mano como antes, y eso es justo lo
 * que no puede pasar: una consulta fuera de la tabla es una segunda via, y una
 * segunda via es la que se queda sin anunciar.
 *
 * Se declara aqui y no se incluye @c lsp_server.h por no cerrar un ciclo: el
 * servidor conoce las filas, las filas no tienen por que conocerlo a el.
 */
struct HostOps {
    virtual ~HostOps() = default;

    /**
     * @brief Compila a disco y devuelve el parte.
     * @param uri     Documento (o su proyecto).
     * @param project Si se compila el proyecto entero en vez del fichero.
     * @param params  Los parametros crudos, que el driver de compilacion lee
     *                con su propio vocabulario (salida, formato, objetivo).
     */
    virtual nlohmann::json compile(const std::string &uri, bool project,
                                   const nlohmann::json &params) = 0;

    /// El simbolo bajo el cursor: nombre, categoria, firma y documentacion.
    virtual nlohmann::json symbol_info(const std::string &uri, uint32_t line,
                                       uint32_t character) = 0;
};

/**
 * @brief Lo que un manejador del inspector necesita para contestar.
 *
 * Hereda de @c query::Ctx para que la fila sea la misma sea quien sea el que
 * pregunta -- el editor hoy, otro transporte manana --.  Lo que cambia por cara
 * es QUIEN rellena esto, no la consulta.
 */
struct InspectorCtx : query::Ctx {
    Inspector *inspector = nullptr;
    DocumentStore *docs = nullptr;
    /// Lo que solo sabe contestar el servidor.  @see HostOps
    HostOps *host = nullptr;
    /**
     * @brief El objetivo ya resuelto, si la fila declara @c Needs::Target.
     *
     * Lo arma el transporte a partir de los parametros del paquete, no cada
     * manejador: armarlo quince veces es quince sitios donde olvidarse de
     * `cpu`.
     */
    InspectTarget target;
};

/**
 * @brief Da de alta las consultas del inspector en el registro.
 *
 * Idempotente por construccion: se llama una vez al arrancar el servidor.
 */
void register_inspector_queries();

} // namespace lsp

#endif // LSP_QUERIES_H
