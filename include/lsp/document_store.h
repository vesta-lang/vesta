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
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace lsp {

/**
 * @struct Position
 * @brief Posicion LSP: linea 0-based + caracter 0-based en unidades UTF-16.
 */
struct Position {
    uint32_t line = 0; ///< Linea, 0-based.
    uint32_t character =
        0; ///< Caracter dentro de la linea, en UTF-16, 0-based.
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
 * @brief Conversion INVERSA: de una posicion LSP a un offset de byte absoluto
 *        dentro del texto completo del documento.
 *
 * Necesaria para la navegacion (hover/definition/references): dada la
 * posicion del cursor (linea 0-based + caracter UTF-16 0-based) localiza el
 * byte exacto del fuente, lo que permite encontrar el token/identificador
 * que esta bajo el cursor.
 *
 * Recorre el texto contando saltos @c \n para situar la linea y luego
 * decodifica UTF-8 dentro de esa linea acumulando unidades UTF-16 hasta
 * alcanzar @p character.  Robusto frente a posiciones fuera de rango: la
 * linea se clampa al numero de lineas y el caracter al final de su linea.
 *
 * @param text      Texto completo del documento.
 * @param line      Linea 0-based.
 * @param character Caracter 0-based en unidades UTF-16 dentro de la linea.
 * @return Offset de byte 0-based desde el inicio del texto.  Si la linea esta
 *         fuera de rango devuelve @c text.size().
 */
uint32_t lsp_position_to_byte_offset(const std::string &text, uint32_t line,
                                     uint32_t character);

/**
 * @brief Conversion INVERSA complementaria: de un offset de byte absoluto a la
 *        posicion LSP (linea 0-based + caracter UTF-16 0-based).
 *
 * Usada para construir los rangos LSP de las definiciones/referencias a
 * partir del @c offset/@c length en bytes que reportan los tokens del lexer,
 * preservando la exactitud con UTF-8 multibyte y plano astral.
 *
 * @param text        Texto completo del documento.
 * @param byte_offset Offset 0-based en bytes.
 * @param out_line    Salida: linea 0-based.
 * @param out_char    Salida: caracter 0-based en UTF-16.
 */
void byte_offset_to_lsp_position(const std::string &text, size_t byte_offset,
                                 uint32_t &out_line, uint32_t &out_char);

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
     * @brief El texto completo de un documento, como INSTANTANEA compartida.
     *
     * Ni referencia ni copia.  Referencia no vale: el editor puede sustituir
     * ese texto mientras otro hilo lo lee, y quedaria apuntando a memoria
     * liberada.  Copia tampoco: medido en este toolchain, buscar y copiar un
     * fichero de 40 KB cuesta 3.016 ns, mientras que el cerrojo que hace falta
     * para mirar el mapa cuesta 9 -- 331 veces menos --.  Copiar seria pagar lo
     * caro para ahorrarse lo barato.
     *
     * El texto es INMUTABLE una vez publicado: cambiar un documento no lo
     * reescribe, pone otro en su sitio.  Asi quien tenga esta instantanea sigue
     * viendo lo mismo mientras la use, sin que nadie tenga que esperarle.
     *
     * @param uri URI del documento.
     * @return El texto; nunca nulo (vacio si el documento no esta abierto).
     */
    std::shared_ptr<const std::string> text(const std::string &uri) const;

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

    /**
     * @brief Cuantas lineas tiene un documento.
     *
     * Recorrerlo hasta que @ref line devuelva vacio no vale: una linea en
     * blanco tambien devuelve vacio, asi que quien lo intentaba tenia que
     * adivinar el final mirando varias seguidas.  Preguntarlo es exacto.
     *
     * @param uri URI del documento.
     * @return Numero de lineas; 0 si no esta abierto.
     */
    uint32_t line_count(const std::string &uri) const;

    /**
     * @brief Las URIs de los documentos abiertos.
     *
     * Hace falta para volver a publicar diagnosticos de TODOS cuando cambia
     * algo que afecta a la compilacion entera -- el objetivo, por ejemplo --,
     * porque lo publicado antes hablaba de otra maquina.
     *
     * @return Copia de las URIs, sin orden garantizado.
     */
    std::vector<std::string> open_uris() const;

  private:
    /// URI -> texto inmutable.  El puntero compartido es lo que permite que
    /// leerlo no cueste copiarlo y que sustituirlo no le quite el suelo a quien
    /// lo estaba leyendo.
    std::unordered_map<std::string, std::shared_ptr<const std::string>> docs_;
    /// Cubre SOLO el mapa -- mirar y guardar un puntero --, nunca el texto.
    /// Nueve nanosegundos sin disputa, frente a los millones que cuesta lo que
    /// se hace despues con lo que se ha sacado.
    mutable std::mutex m_;
};

} // namespace lsp

#endif // VESTA_LSP_DOCUMENT_STORE_H
