/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/effects/bounds.h
 * @brief Accesos que se salen DEMOSTRABLEMENTE de su region.
 *
 * Junta las dos mitades que el modelo ya tiene: donde toca una operacion (raiz
 * + offset + ancho) y hasta donde llega el objeto (la extension de su region).
 *
 * Vive aparte del informe A PROPOSITO.  El veredicto lo consultan DOS sitios --
 * `--analyze`, que lo enseña con su prueba, y la compilacion, que lo convierte
 * en un diagnostico -- y dos copias del mismo criterio acabarian discrepando a
 * la primera op nueva.  Un hecho, un productor.
 */
#ifndef ANALYSIS_EFFECTS_BOUNDS_H
#define ANALYSIS_EFFECTS_BOUNDS_H

#include "analysis/effects/effect_analysis.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ir {
struct IrModule;
}

namespace analysis {
namespace effects {

/**
 * @brief Un acceso que se sale de su region, con lo necesario para explicarlo.
 *
 * Lleva su PRUEBA (region, hueco, tramo accedido): un veredicto sin derivacion
 * obliga a rehacer el razonamiento a mano, y aqui el analisis ya la tiene.
 */
struct BoundsViolation {
    std::string function;      ///< Funcion donde ocurre.
    uint32_t    line = 0;      ///< Linea del fuente.
    bool        write = false; ///< true = escritura, false = lectura.
    int32_t     width = 0;     ///< Bytes del acceso.
    int64_t     off = 0;       ///< Offset desde la raiz.
    int64_t     limite = 0;    ///< Hueco reservado del objeto.
    int64_t     objeto = 0;    ///< Tamano logico del objeto.
    std::string region;        ///< Nombre de la region (`stack#3`, `heap#7`).
};

/**
 * @brief Comprueba TODAS las funciones del modulo.
 *
 * Solo afirma lo DEMOSTRADO: extension constante y offset exacto, y contra el
 * HUECO RESERVADO -- ensanchar un acceso dentro del hueco es legitimo (asi
 * copia el compilador los agregados), salirse de el no lo es nunca.  Con una
 * extension simbolica se calla: se sabe QUE valor manda, no cuanto vale.
 *
 * @param mod Modulo a comprobar.
 * @param ea  Motor de efectos YA construido sobre @p mod, para quien vaya a
 *            preguntarle mas cosas al mismo modulo.  Sin el se construye uno
 *            aqui, lo que significa rehacer el escape y los resumenes de todo
 *            el modulo: medido en un fuente de 300 funciones, esa repeticion
 *            era la mitad de lo que tardaba el informe.
 */
std::vector<BoundsViolation> check_region_bounds(const ir::IrModule &mod,
                                                 EffectAnalysis *ea = nullptr);

} // namespace effects
} // namespace analysis

#endif // ANALYSIS_EFFECTS_BOUNDS_H
