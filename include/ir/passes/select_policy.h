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

#include <cstdint>

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
 * @param fn          Funcion SSA (para rastrear la definicion de @p cond).
 * @param cond        Valor booleano del branch.
 * @param source_line Linea fuente del branch (para el predictor de perfil/PGO);
 *                    0 si no se conoce.
 * @return P(mispredict) en [0,1]; 0.25 si ningun predictor reconoce el patron.
 */
double estimate_p_mispredict(const IrFunction &fn, IrValueId cond,
                             uint32_t source_line);

/**
 * @brief Decide si conviene la forma SELECT frente al BRANCH (modelo de coste).
 *
 * @param fn                  Funcion SSA.
 * @param cond                Valor booleano del branch.
 * @param source_line         Linea fuente del branch (predictor de perfil).
 * @param cost_true           Coste (aprox. latencia) de la rama true.
 * @param cost_false          Coste de la rama false.
 * @param result_loop_carried true si el valor resultante realimenta una
 *                            recurrencia de loop (cmov en el camino critico).
 * @return true si el modelo prefiere SELECT; false si prefiere el branch.
 */
bool prefer_select(const IrFunction &fn, IrValueId cond, uint32_t source_line,
                   int cost_true, int cost_false, bool result_loop_carried);

/// @brief ISA destino, para parametrizar el coste del SELECT (rol
///        TargetPredictor).  El SELECT es una primitiva SEMANTICA: cada backend
///        lo baja distinto (x86 @c cmov, ARM64 @c csel, RISC-V secuencia sin
///        salto), y su coste relativo al branch varia por ISA.
enum class TargetIsa {
    Generic, ///< x86-64 generico (default)
    X86_64,  ///< cmov ~2c, con dependencia de flags
    ARM64,   ///< csel ~1c, sin flag-hazard -> select mas favorable
    RISCV    ///< sin cmov nativo -> secuencia mas cara -> select menos favorable
};

/**
 * @brief Ajusta el modelo de coste (latencia del cmov/csel + penalty de fallo)
 *        segun la ISA destino.  Multi-ISA: el mismo IR de SELECT se evalua con
 *        el coste de la microarquitectura para la que se compila.
 */
void set_target_isa(TargetIsa isa);

/**
 * @brief Override fino del modelo de coste (ciclos).  0 = no cambiar ese campo.
 */
void set_target_cost_model(double cmov_latency, double mispredict_penalty);

/**
 * @brief Carga un perfil de branches (PGO) indexado por linea fuente.
 *
 * Formato de texto, una entrada por linea: @c "<source_line> <taken> <nt>".
 * P(mispredict) = min(taken,nt)/(taken+nt).  Alimenta @c predict_profile, que
 * DOMINA a los predictores estructurales cuando hay dato para esa linea.  Se
 * puede cargar tambien via la variable de entorno @c VESTA_BRANCH_PROFILE.
 * @return numero de entradas cargadas (0 si el archivo no existe/vacio).
 */
int load_branch_profile(const char *path);

/**
 * @brief Inserta/actualiza una entrada del perfil de branches por linea (mismo
 *        almacen que @c load_branch_profile / @c predict_profile).  Lo usa el
 *        auto-PGO del JIT: al recompilar una funcion caliente, vuelca el perfil
 *        de runtime (contadores del profiler mapeados a linea via debug_info) a
 *        este almacen para que la if-conversion re-decida con datos medidos.
 */
void set_branch_profile_entry(uint32_t source_line, double p_mispredict);

} // namespace ir

#endif // IR_PASSES_SELECT_POLICY_H
