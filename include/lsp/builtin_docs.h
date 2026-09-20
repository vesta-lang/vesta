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
 * Los builtins (print, type.size, str_concat, ...) no se declaran en el codigo
 * del usuario, por lo que el indexador de simbolos NO los ve; esta tabla
 * suple su documentacion.
 *
 * @par De donde sale el texto
 * De @c catalog/builtin_docs.toml, que lo tiene en TODOS los idiomas, igual
 * que los diagnosticos.  Es texto que lee una persona, y una persona lee en
 * el suyo; escrito a mano aqui solo podia estar en uno.  El idioma activo lo
 * decide @c vx::diag::current_language, que es el mismo criterio para todo el
 * proyecto -- y se busca por CoDIGO ISO, no por indice, para que los dos
 * catalogos puedan tener idiomas distintos sin cruzarse --.
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

/**
 * @brief Devuelve todos los nombres de builtins documentados (orden estable).
 *
 * Fuente unica para el completado del LSP: el handler de completion une esta
 * lista con las palabras clave y los simbolos del proyecto.  Mantenerla junto
 * a la tabla de docs evita que el completado y el hover diverjan.
 * @return Referencia estable al vector de nombres.
 */
const std::vector<std::string> &all_builtin_names();

// --- La tabla GENERADA (catalog/builtin_docs.toml) ---------------------------
//
// Es lo que hay debajo de las dos funciones de arriba.  Se expone porque la
// construccion de la tabla por idioma vive en el .cpp, no en el generador: lo
// generado son DATOS, y elegir idioma es una decision.

/**
 * @struct BuiltinDocView
 * @brief Una entrada de la tabla generada, sin copiar nada.
 *
 * Todo son punteros a `.rodata`: la tabla no reserva memoria al arrancar.
 */
struct BuiltinDocView {
    const char *name = nullptr;      ///< Nombre del builtin.
    const char *sig = nullptr;       ///< Firma; igual en todos los idiomas.
    const char *const *doc = nullptr; ///< La explicacion, una por idioma.
    int doc_count = 0;                ///< Cuantos idiomas trae @c doc.
};

/// Los idiomas de la tabla generada (codigos ISO).  @p out_n recibe el conteo.
const char *const *builtin_doc_languages(int *out_n);

/// Cuantas entradas tiene la tabla generada.
int builtin_doc_count();

/// La entrada @p idx de la tabla generada.  Falso si el indice no vale.
bool builtin_doc_at(int idx, BuiltinDocView *out);

/// Busca por nombre exacto en la tabla generada (busqueda binaria).
bool builtin_doc_find(const char *name, BuiltinDocView *out);

} // namespace lsp

#endif // VESTA_LSP_BUILTIN_DOCS_H
