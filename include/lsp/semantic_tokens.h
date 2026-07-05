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
 * @file semantic_tokens.h
 * @brief Resaltado semantico (semantic tokens) del LSP de Vesta.
 *
 * Implementa @c textDocument/semanticTokens/full: lexa el documento con el
 * lexer de Vesta, clasifica cada token (keyword, type, class, function,
 * literal, operador, comentario, ...) y lo codifica en el formato plano de
 * quintetos que exige el protocolo:
 *
 *   [deltaLine, deltaStartChar, length, tokenType, tokenModifiers]
 *
 * Donde @c deltaStartChar y @c length van en UNIDADES UTF-16 (igual que el
 * resto del LSP).  Un token que cruzaria varias lineas (comentario de
 * bloque) se PARTE en un token por linea, porque el protocolo no admite
 * tokens multilinea.
 *
 * El enriquecimiento de identificadores (saber si un nombre es una clase,
 * struct, enum, alias o funcion declarada) se hace consultando los sets de
 * nombres del @c DocAnalysis, poblados por el motor de analisis.
 */

#ifndef VESTA_LSP_SEMANTIC_TOKENS_H
#define VESTA_LSP_SEMANTIC_TOKENS_H

#include <cstdint>
#include <string>
#include <vector>

namespace lsp {

struct DocAnalysis; // fwd (lsp/analysis_engine.h)

/**
 * @brief Indices en la leyenda de @c tokenTypes anunciada en @c initialize.
 *
 * El orden DEBE coincidir exactamente con el array
 * @c semantic_token_types() que se publica en las capacidades, porque el
 * cliente mapea por indice numerico.
 */
enum class SemTokenType : uint32_t {
    Namespace = 0,
    Type,
    Class,
    Enum,
    Interface,
    Struct,
    TypeParameter,
    Parameter,
    Variable,
    Property,
    EnumMember,
    Function,
    Method,
    Keyword,
    Modifier,
    Comment,
    String,
    Number,
    Operator,
    Macro,
    // Tipos NO estandar propios de Vesta (la leyenda es custom; el cliente
    // mapea por nombre, asi que anyadirlos al final es seguro):
    EscapeSequence, ///< \n \t \xHH \uHHHH y secuencias ANSI dentro de strings.
    Interpolation,  ///< delimitadores ${ y } de la interpolacion de strings.
    Register        ///< registros de CPU (rax, xmm0, ...) en bloques asm.
    // Las INSTRUCCIONES de asm reusan tipos existentes para diferenciarlas por
    // categoria (mismo color que ya tienen): aritmeticas->Function,
    // logicas/bit->Macro, control de flujo->Keyword, movimiento/datos->Type.
};

/**
 * @brief Bits de la leyenda de @c tokenModifiers.
 *
 * El indice de cada modificador en el array publicado es su numero de bit;
 * el campo @c tokenModifiers de cada quinteto es la mascara OR de los bits
 * activos.  Esta fase emite pocos modificadores (los necesarios); el resto
 * queda reservado para fases futuras.
 */
enum class SemTokenModifier : uint32_t {
    Declaration = 1u << 0,
    Definition = 1u << 1,
    Readonly = 1u << 2,
    Static = 1u << 3,
    Abstract = 1u << 4,
    Deprecated = 1u << 5
};

/**
 * @brief Devuelve la leyenda de tipos de token (orden estable = indices).
 * @return Vector de nombres LSP estandar de @c tokenTypes.
 */
const std::vector<std::string> &semantic_token_types();

/**
 * @brief Devuelve la leyenda de modificadores de token (orden = numero de
 *        bit).
 * @return Vector de nombres LSP estandar de @c tokenModifiers.
 */
const std::vector<std::string> &semantic_token_modifiers();

/**
 * @brief Calcula los semantic tokens de un documento.
 *
 * Lexa @p text con el lexer de Vesta, escanea aparte los comentarios (el
 * lexer los descarta), clasifica cada elemento y produce el array plano de
 * datos LSP (longitud multiplo de 5, deltas relativos, UTF-16).
 *
 * Robusto: cualquier fallo interno (excepcion del lexer) se captura y la
 * funcion devuelve los tokens producidos hasta ese punto (posiblemente
 * vacios).  Nunca lanza.
 *
 * @param text     Texto fuente del documento.
 * @param filename Nombre logico del fichero (para el lexer).
 * @param analysis Analisis del documento (sets de nombres declarados) para
 *                 enriquecer los identificadores.  Puede ser nullptr: en
 *                 ese caso los identificadores se clasifican como variable.
 * @return Array plano de uint32 con quintetos
 *         @c [deltaLine,deltaStartChar,length,tokenType,tokenModifiers].
 */
std::vector<uint32_t> compute_semantic_tokens(const std::string &text,
                                              const std::string &filename,
                                              const DocAnalysis *analysis);

} // namespace lsp

#endif // VESTA_LSP_SEMANTIC_TOKENS_H
