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
 * @file param_hints.h
 * @brief Calculo de "parameter hints" (inlay) del LSP de Vesta.
 *
 * Para cada llamada a una funcion conocida @c f(a, b, c) produce un hint con el
 * NOMBRE de cada parametro justo ANTES de su argumento, de modo que el cliente
 * lo pinte como texto fantasma: @c f(x: a, y: b, z: c).  Asi el lector ve el rol
 * de cada argumento sin abrir la firma.
 *
 * Estrategia (robusta, sin recorrer todo el AST):
 *   1. PARSEAR el documento y recolectar, de las declaraciones top-level
 *      (@c FunctionDecl y @c ExternFnDecl), el mapa nombre -> [nombres de sus
 *      parametros].
 *   2. LEXAR el documento y escanear el patron @c IDENT( de una funcion del
 *      mapa que NO sea acceso a miembro (@c .m()) ni @c new T(); balanceando
 *      parentesis/corchetes/llaves se localiza el inicio de cada argumento de
 *      nivel 0 y se emite el hint del parametro correspondiente.
 *
 * Las posiciones se devuelven en coordenadas LSP: linea 0-based y caracter
 * 0-based en UNIDADES UTF-16 (igual que el resto del LSP).
 */

#ifndef VESTA_LSP_PARAM_HINTS_H
#define VESTA_LSP_PARAM_HINTS_H

#include <cstdint>
#include <string>
#include <vector>

namespace lsp {

/// Un parameter hint resuelto a coordenadas LSP.
struct ParamHint {
    uint32_t line = 0;      ///< Linea 0-based.
    uint32_t character = 0; ///< Caracter 0-based en UTF-16 (inicio del arg).
    std::string label;      ///< Texto a mostrar (p.ej. "count:").
};

/**
 * @brief Calcula los parameter hints de un documento Vesta.
 *
 * Robusto: cualquier fallo interno (excepcion del lexer/parser) se captura y la
 * funcion devuelve lo acumulado (posiblemente vacio).  Nunca lanza.
 *
 * @param text     Texto fuente del documento.
 * @param filename Nombre logico del fichero (para el lexer/parser).
 * @return Vector de hints (vacio si no hay llamadas reconocidas).
 */
std::vector<ParamHint> compute_param_hints(const std::string &text,
                                           const std::string &filename);

} // namespace lsp

#endif // VESTA_LSP_PARAM_HINTS_H
