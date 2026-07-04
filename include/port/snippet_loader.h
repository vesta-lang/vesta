/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file snippet_loader.h
 * @brief Carga de snippets de codigo del directorio @c stdlib/port/c.
 *
 * Los snippets `<name>.v.c` contienen el codigo C/C++ ESTATICO de los backends
 * de port (macros, prologos, runtime de excepciones, etc.) para que NO este
 * hardcodeado en el compilador: la mayoria del C/C++ vive en @c stdlib/port y
 * el generador solo aporta la parte data-driven (typedefs, prototipos).
 *
 * El backend C (`CBackend`) y el generador de headers (`c_header_gen`)
 * comparten esta resolucion de directorio + lectura.
 */

#ifndef VX_PORT_SNIPPET_LOADER_H
#define VX_PORT_SNIPPET_LOADER_H

#include <string>

namespace port {

/**
 * @brief Resuelve el directorio @c stdlib/port/c.
 * @param override_dir si no esta vacio, se usa tal cual.
 * @return el directorio encontrado (autodetect desde cwd y desde el path del
 *         ejecutable), o un fallback razonable.
 */
std::string resolve_port_c_dir(const std::string &override_dir = "");

/**
 * @brief Lee el CONTENIDO de un snippet `stdlib/port/c/<name>.v.c`.
 *
 * Elimina la cabecera de metadata (lineas iniciales `// @...`).  No envuelve
 * en comentarios BEGIN/END (eso lo decide el caller).
 *
 * @param name nombre del snippet sin extension.
 * @param override_dir dir explicito opcional.
 * @param[out] ok true si se leyo; false si no se encontro.
 * @return el contenido, o "" si no se encontro (con @p ok=false).
 */
std::string load_snippet_text(const std::string &name,
                              const std::string &override_dir, bool &ok);

} // namespace port

#endif // VX_PORT_SNIPPET_LOADER_H
