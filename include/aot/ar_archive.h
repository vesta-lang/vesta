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
 * @file aot/ar_archive.h
 * @brief Lector portable del formato de archivo estatico Unix @c ar(1) (.a).
 *
 *  AOT.5: permite que el linker propio enlace CUALQUIER libreria estatica
 * (.a) -- p.ej. @c libvesta_gc.a (el GC opt-in) o libc.a -- extrayendo sus
 * miembros (objetos ELF/COFF) en memoria, sin g++/ar externos.
 *
 * Soporta las variantes comunes:
 *   - GNU/SysV: nombres cortos terminados en '/', nombres largos via la tabla
 *     "//" y referencias "/N"; miembro de indice de simbolos "/".
 *   - BSD: nombres largos "#1/<len>" con el nombre real prefijado en los datos;
 *     indice "__.SYMDEF".
 *
 * El indice de simbolos (cuando existe) se expone para que el linker haga
 * "pull" perezoso: solo extrae los miembros que definen un simbolo realmente
 * referenciado (semantica estandar de linker), sin parsear el archivo entero.
 */
#ifndef VESTA_AOT_AR_ARCHIVE_H
#define VESTA_AOT_AR_ARCHIVE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aot {

/// Miembro (objeto) dentro de un archivo .a.  Los offsets indexan el buffer
/// completo del .a pasado a @c ar_parse.
struct ArMember {
    std::string name;         ///< Nombre resuelto (p.ej. "gc_heap.cpp.obj");
                              ///< en un thin archive es la RUTA del objeto.
    size_t header_offset = 0; ///< Offset de la cabecera de 60 bytes
    size_t data_offset = 0;   ///< Offset del primer byte de datos (thin: 0)
    size_t size = 0;          ///< Tamano de los datos del objeto (thin: 0)
    bool is_thin = false;     ///< thin archive: los datos estan en un fichero
                              ///< externo (la ruta es @c name), no en el .a.
};

/// Entrada del indice de simbolos: nombre -> indice del miembro en el vector
/// devuelto por @c ar_parse (o -1 si el offset no casa con ningun miembro).
struct ArSymbol {
    std::string name;
    int member_index = -1;
};

/// @return true si @p buf empieza con el magic de un archivo ar ("!<arch>\n").
bool ar_is_archive(const std::vector<uint8_t> &buf);

/**
 * @brief Parsea los miembros-objeto de un archivo .a.
 *
 * Resuelve nombres largos (GNU "//" + "/N", BSD "#1/len").  Omite los miembros
 * especiales de servicio (tabla de nombres "//", indice de simbolos "/" y
 * "__.SYMDEF").  Si el archivo trae indice de simbolos, lo devuelve en
 * @p symbols mapeado al indice de miembro correspondiente (para "pull"
 * perezoso); si no, @p symbols queda vacio y el linker debe escanear los
 * symtab de los miembros.
 *
 * @param buf      Contenido completo del .a.
 * @param members  [out] miembros-objeto en orden de aparicion.
 * @param symbols  [out] indice de simbolos (puede quedar vacio).
 * @param err      [out] mensaje de error si devuelve false.
 * @return true si el parseo fue correcto.
 */
bool ar_parse(const std::vector<uint8_t> &buf, std::vector<ArMember> &members,
              std::vector<ArSymbol> &symbols, std::string &err);

} // namespace aot

#endif // VESTA_AOT_AR_ARCHIVE_H
