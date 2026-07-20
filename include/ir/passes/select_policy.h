/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file select_policy.h
 * @brief Politica de rentabilidad de la if-conversion (modelo MIXTO).
 *
 * Decide, para un diamante candidato, si conviene la forma SELECT (sin salto,
 * cmov) o mantener el BRANCH.  NO es un clasificador que decide por reglas: es
 * un MODELO DE COSTE alimentado por PREDICTORES ESPECIALIZADOS, cada uno experto
 * en un dominio, que INFORMAN la probabilidad de fallo de prediccion.  Los
 * predictores que no reconocen el patron no tocan la puntuacion (Unknown).  Asi
 * el sistema es extensible: anadir un predictor nuevo (uarch, PGO, ...) no toca
 * el resto.
 *
 * Modelo (para un select cuyo resultado realimenta una recurrencia de loop, el
 * caso que decide): el cmov queda en el camino critico CADA iteracion con
 * latencia @c CMOV_LAT; el branch paga @c P(mispredict)*penalty.  Se convierte
 * sii @c score(select) < @c score(branch).  Para ramas triviales loop-carried
 * eso equivale a @c CMOV_LAT < P*penalty (cruce en P~0.13 con penalty~15).
 *
 * NOTA de integracion: la decision se hornea en el IR compartido por los tres
 * backends.  Los predictores (predecibilidad ESTRUCTURAL) son target-neutrales
 * y el modelo de coste usa parametros genericos; el refinamiento por-target
 * (que el backend DESHAGA un select a branch segun su microarquitectura) es
 * trabajo futuro.  El PGO (cuando exista) domina via un predictor de perfil.
 */

#ifndef IR_PASSES_SELECT_POLICY_H
#define IR_PASSES_SELECT_POLICY_H

#include "ir/ssa_ir.h"

namespace ir {

/// @brief Clase de comportamiento de un branch (la produce un predictor).
enum class BranchClass {
    AlmostAlwaysTaken, ///< casi siempre tomado (P_mis muy baja)
    AlmostNeverTaken,  ///< casi nunca tomado (P_mis muy baja)
    LoopExit,          ///< condicion de salida de loop (predecible)
    DataDependent,     ///< depende de datos (bit-test de valor variable)
    HashLike,          ///< derivado de hash/mezcla (impredecible)
    RNGLike,           ///< derivado de xorshift/PRNG (impredecible ~50/50)
    Unknown            ///< el predictor no reconoce el patron
};

/// @brief Resultado de un predictor especializado.
struct PredictorResult {
    bool known = false;         ///< false = Unknown, no aporta informacion
    BranchClass cls = BranchClass::Unknown;
    double p_mispredict = 0.25; ///< estimacion de P(fallo de prediccion)
    double confidence = 0.0;    ///< 0..1; se elige el predictor mas confiado
};

/**
 * @brief Estima P(mispredict) para la condicion @p cond ejecutando todos los
 *        predictores especializados y quedandose con el mas confiado.
 * @param fn   Funcion SSA (para rastrear la definicion de @p cond).
 * @param cond Valor booleano del branch.
 * @return P(mispredict) en [0,1]; 0.25 si ningun predictor reconoce el patron.
 */
double estimate_p_mispredict(const IrFunction &fn, IrValueId cond);

/**
 * @brief Decide si conviene la forma SELECT frente al BRANCH (modelo de coste).
 *
 * @param fn                  Funcion SSA.
 * @param cond                Valor booleano del branch.
 * @param cost_true           Coste (aprox. latencia) de la rama true.
 * @param cost_false          Coste de la rama false.
 * @param result_loop_carried true si el valor resultante realimenta una
 *                            recurrencia de loop (cmov en el camino critico).
 * @return true si el modelo prefiere SELECT; false si prefiere el branch.
 */
bool prefer_select(const IrFunction &fn, IrValueId cond, int cost_true,
                   int cost_false, bool result_loop_carried);

} // namespace ir

#endif // IR_PASSES_SELECT_POLICY_H
