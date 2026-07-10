/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file comptime_collect.h
 * @brief Recolector del "conjunto comptime" de un modulo (P1 fase 1).
 *
 * Identifica TODO el codigo que se evalua en compile-time (comptime fns,
 * @Macros, globales comptime/const) y sus dependencias TRANSITIVAS (funciones
 * no-comptime que ese codigo llama).  Es el primer paso de la migracion "todo
 * comptime corre en la ComptimeVM": el conjunto recolectado es lo que se
 * compilara en un ARTEFACTO SEPARADO y se cargara en la ComptimeVM antes del
 * modulo, para resolver todos los call sites comptime por ejecucion real (sin
 * el tree-walker AST y sin el path "block -> module_init").
 *
 * Analisis PURO: no muta el AST ni el estado del compilador.
 */

#ifndef VX_COMPTIME_COLLECT_H
#define VX_COMPTIME_COLLECT_H

#include <ostream>
#include <string>
#include <vector>

#include "vx/ast.h"

namespace vx {

/**
 * @brief Descriptor del conjunto de codigo comptime de un modulo.
 *
 * Las cuatro listas son disjuntas por categoria; @c helper_deps son las
 * funciones NO-comptime que el codigo comptime necesita para ejecutarse en la
 * ComptimeVM (deben viajar en el mismo artefacto).
 */
struct ComptimeUnit {
    std::vector<std::string> comptime_fns;    ///< `comptime fn` (no @Macro).
    std::vector<std::string> macros;          ///< `@Macro`.
    std::vector<std::string> comptime_consts; ///< globales `comptime`/`const`.
    std::vector<std::string> helper_deps; ///< fns no-comptime llamadas por el
                                          ///< codigo comptime (transitivo).

    /// @return true si el modulo no tiene nada comptime (el artefacto seria
    /// vacio y P1 no aplica).
    bool empty() const {
        return comptime_fns.empty() && macros.empty() &&
               comptime_consts.empty();
    }
};

/**
 * @brief Recolecta el conjunto comptime del modulo.
 *
 * Clasifica las decls top-level y hace un BFS sobre los cuerpos del codigo
 * comptime para arrastrar las funciones llamadas (dependencias) que no son
 * ellas mismas comptime.  El cierre transitivo garantiza que el artefacto
 * separado sea auto-suficiente.
 *
 * @param mod Modulo ya parseado (post-namespace-flatten preferentemente, para
 *            que los nombres esten mangled y las llamadas resuelvan).
 * @return El conjunto comptime.  @c empty() si no hay nada que compilar aparte.
 */
ComptimeUnit collect_comptime_unit(const ast::ModuleNode &mod);

/**
 * @brief Vuelca el conjunto a un stream, legible, para diagnostico.
 *
 * Activado por el driver via la env var @c VESTA_DUMP_COMPTIME_UNIT=1.
 */
void dump_comptime_unit(const ComptimeUnit &u, std::ostream &os);

} // namespace vx

#endif // VX_COMPTIME_COLLECT_H
