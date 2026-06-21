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
 * @file document_store.h
 * @brief Almacen de documentos abiertos + conversion de posiciones.
 *
 * El servidor mantiene el texto completo de cada documento abierto,
 * indexado por su URI (sincronizacion "full text": cada cambio reemplaza
 * el contenido entero).
 *
 * El frontend del compilador reporta posiciones como (linea 1-based,
 * columna 1-based EN BYTES).  El LSP exige (linea 0-based, caracter
 * 0-based EN UNIDADES UTF-16).  Aqui viven los helpers que traducen entre
 * ambos sistemas usando el texto del documento, imprescindibles para que
 * los rangos caigan correctamente con UTF-8 multibyte y con caracteres
 * fuera del plano basico (que ocupan dos unidades UTF-16).
 */

#ifndef VESTA_LSP_DOCUMENT_STORE_H
#define VESTA_LSP_DOCUMENT_STORE_H

#include <cstdint>
#include <string>
#include <unordered_map>

namespace lsp {

/**
 * @struct Position
 * @brief Posicion LSP: linea 0-based + caracter 0-based en unidades UTF-16.
 */
struct Position {
    uint32_t line = 0;      ///< Linea, 0-based.
    uint32_t character = 0; ///< Caracter dentro de la linea, en UTF-16, 0-based.
};

/**
 * @brief Convierte el numero de columna 1-based en BYTES del compilador a la
 *        unidad UTF-16 0-based que exige el LSP.
 *
 * Decodifica los bytes UTF-8 de @p line_text hasta alcanzar el offset de
 * byte indicado (= @p byte_column_1based - 1) y cuenta cuantas unidades
 * UTF-16 se habrian emitido.  Los caracteres del plano astral (>= U+10000)
 * cuentan como dos unidades (par surrogate).
 *
 * Robusto frente a entradas malformadas o columnas fuera de rango: nunca
 * lee mas alla del final de la linea y trata bytes de continuacion sueltos
 * de forma conservadora.
 *
 * @param line_text         Texto de la linea (sin el salto de linea final).
 * @param byte_column_1based Columna 1-based en bytes (1 = inicio de linea).
 * @return Caracter 0-based en unidades UTF-16.
 */
uint32_t byte_column_to_utf16(const std::string &line_text,
                              uint32_t byte_column_1based);

/**
 * @class DocumentStore
 * @brief Mapa URI -> texto completo del documento.
 *
 * Sincronizacion full-text: @c open y @c update reemplazan el contenido
 * entero.  Provee acceso al texto y la extraccion de una linea concreta
 * para la conversion de posiciones.
 */
class DocumentStore {
  public:
    /**
     * @brief Inserta o reemplaza el contenido de un documento.
     * @param uri  URI del documento (clave).
     * @param text Texto completo nuevo.
     */
    void open(const std::string &uri, std::string text);

    /**
     * @brief Reemplaza el contenido de un documento ya abierto.
     * @param uri  URI del documento.
     * @param text Texto completo nuevo (full sync).
     */
    void update(const std::string &uri, std::string text);

    /**
     * @brief Elimina un documento del almacen.
     * @param uri URI del documento a cerrar.
     */
    void close(const std::string &uri);

    /**
     * @brief Indica si un documento esta abierto.
     * @param uri URI a consultar.
     * @return true si el documento esta en el almacen.
     */
    bool has(const std::string &uri) const;

    /**
     * @brief Devuelve el texto completo de un documento.
     * @param uri URI del documento.
     * @return Referencia al texto; cadena vacia estatica si no existe.
     */
    const std::string &text(const std::string &uri) const;

    /**
     * @brief Extrae la linea @p line_0based de un documento (sin el salto).
     *
     * Cuenta saltos @c \n para localizar la linea.  Devuelve cadena vacia
     * si el documento no existe o la linea esta fuera de rango.  Usado por
     * la conversion de columnas byte->UTF-16.
     *
     * @param uri        URI del documento.
     * @param line_0based Indice de linea 0-based.
     * @return Contenido de la linea sin el terminador.
     */
    std::string line(const std::string &uri, uint32_t line_0based) const;

  private:
    std::unordered_map<std::string, std::string> docs_; ///< URI -> texto.
};

} // namespace lsp

#endif // VESTA_LSP_DOCUMENT_STORE_H
