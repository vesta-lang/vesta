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
 * @file diag_catalog.h
 * @brief Catalogo multi-idioma de diagnosticos del compilador.
 *
 * Toda cadena visible del compilador (errores, warnings, notas) sale por un
 * CATaLOGO indexado por un CoDIGO estable (VXNNNN).  El texto se traduce por
 * idioma sin tocar el codigo: el codigo es lo permanente (documentacion,
 * busquedas, herramientas lo referencian), el texto es intercambiable.
 *
 * El catalogo se GENERA a C++ autocontenido desde @c catalog/diagnostics.toml
 * (ver tools/import/gen_diag_catalog.py) -> sin ficheros externos en runtime.
 * El idioma activo se elige del ENTORNO (@c VESTA_LANG > @c LANG del sistema,
 * fallback al primer idioma del catalogo).  Los emisores pasan DATOS (args), no
 * texto formateado; el formateo (sustitucion de @c {0},{1},...) ocurre al
 * imprimir, en el idioma activo -> el mismo diagnostico se puede reformatear en
 * cualquier idioma o volcar a un formato maquina (JSON/SARIF) sin perder los
 * datos.
 *
 * NUNCA se traducen: keywords del lenguaje, atributos, nombres de tipos,
 * identificadores del usuario ni los codigos de diagnostico.
 */

#ifndef VX_DIAG_CATALOG_H
#define VX_DIAG_CATALOG_H

#include <string>
#include <vector>

namespace vx {
namespace diag {

// --- Accesores a la tabla GENERADA (diag_catalog_gen.cpp) ---

/// Lista de idiomas del catalogo (codigos ISO).  @p out_n recibe el conteo.
const char *const *catalog_languages(int *out_n);

/// Numero de entradas (codigos) del catalogo.
int catalog_entry_count();

/// Plantilla del @p code en el idioma @p lang (indice), o nullptr si no existe.
const char *catalog_template(const char *code, int lang);

// --- Seleccion de idioma ---

/// Numero de idiomas disponibles.
int language_count();

/// Codigo ISO del idioma @p idx (p.ej. "en"), o "" si el indice no es valido.
const char *language_code(int idx);

/// Indice del idioma cuyo codigo ISO es @p code (p.ej. "es"), o -1 si no esta.
/// Acepta formas como "es_ES.UTF-8" (usa el prefijo antes de '_'/'.').
int language_index(const std::string &code);

/// Indice del idioma segun el ENTORNO: @c VESTA_LANG, luego @c LC_ALL / @c
/// LANG. Si ninguno se reconoce, devuelve 0 (el primer idioma del catalogo,
/// fallback).
int language_from_env();

/// Fija el idioma activo global (indice).  Fuera de rango -> se ignora.
void set_language(int idx);

/// Idioma activo global (indice).  Por defecto 0 hasta que se llame
/// set_language.
int current_language();

// --- Formateo ---

/// @c true si el catalogo contiene @p code.
bool has_code(const std::string &code);

/// Formatea el mensaje del @p code en el idioma ACTIVO con @p args.  Si falta
/// la traduccion, cae al idioma 0; si el codigo no existe, devuelve el propio
/// code.
std::string format(const std::string &code,
                   const std::vector<std::string> &args = {});

/// Igual pero en el idioma @p lang explicito.
std::string format(const std::string &code, int lang,
                   const std::vector<std::string> &args);

} // namespace diag
} // namespace vx

#endif // VX_DIAG_CATALOG_H
