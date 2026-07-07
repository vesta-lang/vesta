/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file lsp/builtin_docs.h
 * @brief Tabla central de metadatos de los builtins de Vesta para el LSP.
 *
 * Fuente unica de verdad consumida por:
 *   - @c vesta/symbolInfo (hover): firma + descripcion de que recibe / hace /
 *     retorna el builtin.
 *   - @c compute_param_hints (ghost args): nombres de los parametros para
 *     mostrarlos inline en el call site.
 *
 * Los builtins (print, sizeof, str_concat, ...) no se declaran en el codigo
 * del usuario, por lo que el indexador de simbolos NO los ve; esta tabla
 * suple su documentacion.
 */

#ifndef VESTA_LSP_BUILTIN_DOCS_H
#define VESTA_LSP_BUILTIN_DOCS_H

#include <string>
#include <vector>

namespace lsp {

/**
 * @struct BuiltinDoc
 * @brief Metadatos de un builtin para el hover y los ghost args.
 */
struct BuiltinDoc {
    const char *name;      ///< Nombre del builtin (clave; siempre un literal).
    std::string signature; ///< Firma legible, p.ej. "println(value) -> void".
    std::string doc;       ///< Descripcion (que recibe / hace / retorna).
    std::vector<std::string> params; ///< Nombres de parametros (ghost args).
};

/**
 * @brief Busca un builtin por nombre.
 * @param name Nombre exacto del builtin.
 * @return Puntero estable al metadato, o @c nullptr si no es un builtin.
 */
const BuiltinDoc *lookup_builtin(const std::string &name);

} // namespace lsp

#endif // VESTA_LSP_BUILTIN_DOCS_H
