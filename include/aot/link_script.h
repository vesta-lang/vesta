/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file aot/link_script.h
 * @brief Phase AOT.5 -- script de enlace ESCRITO EN VEX (configurable).
 *
 * El usuario configura el linker con un @c .vex normal (sin sintaxis nueva):
 * define una funcion @c link() que llama a builtins de configuracion.  El
 * linker compila ese @c .vex, lo ejecuta in-process (VM) con los builtins
 * registrados como funciones nativas, y lee la configuracion resultante.  Es
 * intuitivo (es Vex) y potente (logica/bucles/comptime completos para CALCULAR
 * la disposicion).  Builtins disponibles dentro de @c link():
 *
 *   base(u64 addr)            -- direccion de carga del ejecutable
 *   entry(string sym)         -- simbolo de entrada (sin _start sintetico)
 *   stack(u64 bytes)          -- tamano de la pila
 *   section(string nm, u64 a) -- coloca una seccion en una VA fija
 *   section_size(string nm)   -- tamano (bytes) de una seccion ya fusionada
 *   align_up(u64 v, u64 a)    -- redondea v hacia arriba a multiplo de a
 *   debug_build()             -- true si se enlaza en modo debug
 */

#ifndef AOT_LINK_SCRIPT_H
#define AOT_LINK_SCRIPT_H

#include <cstdint>
#include <string>
#include <unordered_map>

namespace aot {

/**
 * @brief Configuracion producida por un link-script Vex (campos opcionales).
 */
struct LinkScriptConfig {
    bool has_base = false;
    uint64_t base = 0;
    bool has_entry = false;
    std::string entry;
    bool has_stack = false;
    uint64_t stack = 0;
    /// VA fija por seccion (de @c section(...)); aplicada por el emisor cuando
    /// soporte placement fijo (los knobs base/entry/stack se aplican ya).
    std::unordered_map<std::string, uint64_t> sections;
};

/**
 * @brief Compila y ejecuta @p script_path (un .vex con @c fn link()) y devuelve
 *        la configuracion del enlace.
 * @param script_path Ruta del .vex de configuracion.
 * @param sec_sizes   Tamanos de las secciones fusionadas (para section_size()).
 * @param debug_build Valor que devolvera el builtin @c debug_build().
 * @param out         [salida] Config leida del script.
 * @param err         [salida] Mensaje de error si falla.
 * @return true en exito.
 */
bool aot_run_link_script(
    const std::string &script_path,
    const std::unordered_map<std::string, uint64_t> &sec_sizes, bool debug_build,
    LinkScriptConfig &out, std::string &err);

} // namespace aot

#endif // AOT_LINK_SCRIPT_H
