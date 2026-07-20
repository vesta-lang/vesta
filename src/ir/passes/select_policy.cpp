/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file select_policy.cpp
 * @brief Implementacion del modelo mixto de rentabilidad de la if-conversion.
 *
 * Ver @c include/ir/passes/select_policy.h.  Estructura:
 *   1. Utilidades: localizar la definicion de un valor, coste de un op.
 *   2. Predictores especializados (cada uno experto en un dominio; devuelven
 *      Unknown si no reconocen el patron).
 *   3. @c estimate_p_mispredict: combina los predictores (el mas confiado gana).
 *   4. @c prefer_select: modelo de coste (score(branch) vs score(select)).
 */

#include "ir/passes/select_policy.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace ir {
namespace {

// --- Perfil de branches (PGO) indexado por linea fuente --------------------
// P(mispredict) real observado en un run instrumentado.  Cuando hay dato para
// la linea de un branch, DOMINA a los predictores estructurales.
std::unordered_map<uint32_t, double> g_branch_profile;

/// @brief Carga perezosa desde @c VESTA_BRANCH_PROFILE la primera vez.
void ensure_profile_loaded() {
    static bool done = false;
    if (done) return;
    done = true;
    const char *p = std::getenv("VESTA_BRANCH_PROFILE");
    if (p && p[0] != '\0') load_branch_profile(p);
}

// --- Parametros del modelo de coste (target-neutrales por ahora) -----------
constexpr double kMispredictPenalty = 15.0; ///< ciclos perdidos por fallo
constexpr double kCmovLatency = 2.0;        ///< latencia del cmov (camino critico)
constexpr double kDefaultPMispredict = 0.25; ///< sin predictor -> incertidumbre

/// @brief Localiza la instruccion que define @p vid, o nullptr.
const IrInstr *find_def(const IrFunction &fn, IrValueId vid) {
    if (vid == IR_NO_VALUE) return nullptr;
    for (const auto &b : fn.blocks)
        for (const auto &ins : b.instrs)
            if (ins.dst == vid) return &ins;
    return nullptr;
}

/// @brief true si @p vid es una constante entera; devuelve su valor en @p out.
bool as_const(const IrFunction &fn, IrValueId vid, uint64_t &out) {
    if (vid == IR_NO_VALUE || static_cast<size_t>(vid) >= fn.values.size())
        return false;
    if (!fn.values[vid].is_const) return false;
    out = fn.values[vid].const_val;
    return true;
}

/// @brief Coste aproximado (latencia) de una op especulable en una rama.
int op_cost(IrOp op) {
    switch (op) {
    case IrOp::CONST:
    case IrOp::MOV:
    case IrOp::SEXT:
    case IrOp::ZEXT:
    case IrOp::TRUNC:
    case IrOp::CAST:
    case IrOp::BITCAST: return 0; // suelen renombrarse / plegarse
    case IrOp::MUL: return 3;
    case IrOp::SELECT: return 2; // cmov anidado
    default: return 1;           // ALU/logica/cmp/shift
    }
}

// =========================================================================
//  Predictores especializados
// =========================================================================

/**
 * @brief Predictor de BIT-TEST de dato (hash/RNG): reconoce la condicion
 *        @c (x & mask_const) == 0  o  @c (x & mask_const) != 0.
 *
 * Para un @p x con distribucion ~uniforme de bits (hash, xorshift, producto de
 * datos), @c P(x & mask == 0) = 2^-popcount(mask).  La mejor prediccion
 * estatica falla @c min(P_true, 1-P_true) de las veces.  Cubre exactamente los
 * dos casos que decidian el diseno: @c (rng&1)!=0 (1 bit -> P_mis=0.5, RNGLike)
 * y @c (seed&7)==0 (3 bits -> P_mis=0.125, casi-nunca-tomado).
 */
PredictorResult predict_data_bittest(const IrFunction &fn, IrValueId cond) {
    PredictorResult r;
    const IrInstr *cmp = find_def(fn, cond);
    if (!cmp) return r;
    if (cmp->op != IrOp::CMP_EQ && cmp->op != IrOp::CMP_NE) return r;
    if (cmp->operands.size() != 2) return r;

    // Un operando debe ser la constante 0; el otro, el resultado de un AND.
    uint64_t cv = 0;
    IrValueId and_side = IR_NO_VALUE;
    if (as_const(fn, cmp->operands[1], cv) && cv == 0)
        and_side = cmp->operands[0];
    else if (as_const(fn, cmp->operands[0], cv) && cv == 0)
        and_side = cmp->operands[1];
    else
        return r;

    const IrInstr *andi = find_def(fn, and_side);
    if (!andi || andi->op != IrOp::AND || andi->operands.size() != 2) return r;

    // La mascara es el operando constante del AND.
    uint64_t mask = 0;
    bool have_mask = false;
    if (as_const(fn, andi->operands[1], mask))
        have_mask = true;
    else if (as_const(fn, andi->operands[0], mask))
        have_mask = true;
    if (!have_mask || mask == 0) return r;

    const int bits = __builtin_popcountll(mask);
    double p_true = 1.0;
    for (int i = 0; i < bits; ++i) p_true *= 0.5; // 2^-bits
    if (cmp->op == IrOp::CMP_NE) p_true = 1.0 - p_true;
    const double p_mis = std::min(p_true, 1.0 - p_true);

    r.known = true;
    r.p_mispredict = p_mis;
    r.confidence = 0.75;
    // 1 bit ~50/50 = RNGLike; varios bits sesgado = casi-nunca/siempre.
    r.cls = (bits == 1) ? BranchClass::RNGLike
                        : (p_true < 0.5 ? BranchClass::AlmostNeverTaken
                                        : BranchClass::AlmostAlwaysTaken);
    return r;
}

/**
 * @brief Predictor de comparacion contra una CONSTANTE de un valor que NO es un
 *        bit-test (p.ej. @c x == K, @c x < K).  Sin mas informacion es una
 *        senal debil de sesgo (las igualdades exactas suelen ser predecibles).
 *        Confianza baja: solo desempata cuando nadie mas sabe.
 */
PredictorResult predict_const_compare(const IrFunction &fn, IrValueId cond) {
    PredictorResult r;
    const IrInstr *cmp = find_def(fn, cond);
    if (!cmp) return r;
    switch (cmp->op) {
    case IrOp::CMP_EQ:
    case IrOp::CMP_NE:
    case IrOp::CMP_LT:
    case IrOp::CMP_GT:
    case IrOp::CMP_LE:
    case IrOp::CMP_GE:
    case IrOp::CMP_ULT:
    case IrOp::CMP_UGT:
    case IrOp::CMP_ULE:
    case IrOp::CMP_UGE: break;
    default: return r;
    }
    if (cmp->operands.size() != 2) return r;
    uint64_t cv = 0;
    const bool has_const =
        as_const(fn, cmp->operands[0], cv) || as_const(fn, cmp->operands[1], cv);
    if (!has_const) return r;
    // Igualdad exacta contra constante: tiende a ser predecible.
    const bool eq = (cmp->op == IrOp::CMP_EQ || cmp->op == IrOp::CMP_NE);
    r.known = true;
    r.p_mispredict = eq ? 0.1 : 0.2;
    r.confidence = 0.3;
    r.cls = eq ? BranchClass::AlmostNeverTaken : BranchClass::DataDependent;
    return r;
}

/**
 * @brief Predictor de PERFIL (PGO): P(mispredict) real observado en un run,
 *        indexado por la linea fuente del branch.  Confianza muy alta: cuando
 *        hay dato medido, DOMINA a los predictores estructurales.
 */
PredictorResult predict_profile(uint32_t source_line) {
    PredictorResult r;
    ensure_profile_loaded();
    if (source_line == 0 || g_branch_profile.empty()) return r;
    auto it = g_branch_profile.find(source_line);
    if (it == g_branch_profile.end()) return r;
    r.known = true;
    r.p_mispredict = it->second;
    r.confidence = 0.95; // el dato medido manda sobre lo estructural
    r.cls = it->second > 0.35 ? BranchClass::DataDependent
            : it->second < 0.05 ? BranchClass::AlmostNeverTaken
                                 : BranchClass::Unknown;
    return r;
}

} // namespace

double estimate_p_mispredict(const IrFunction &fn, IrValueId cond,
                             uint32_t source_line) {
    // Ejecutar todos los predictores; quedarse con el mas confiado.  Anadir un
    // predictor nuevo (Loop/Pointer/Switch/Target) es solo sumarlo aqui; el que
    // no reconoce el patron devuelve Unknown y no aporta.  El de PERFIL domina
    // cuando hay dato medido.
    const PredictorResult preds[] = {
        predict_profile(source_line),
        predict_data_bittest(fn, cond),
        predict_const_compare(fn, cond),
    };
    const PredictorResult *best = nullptr;
    for (const auto &p : preds) {
        if (!p.known) continue;
        if (!best || p.confidence > best->confidence) best = &p;
    }
    return best ? best->p_mispredict : kDefaultPMispredict;
}

bool prefer_select(const IrFunction &fn, IrValueId cond, uint32_t source_line,
                   int cost_true, int cost_false, bool result_loop_carried) {
    const double p_mis = estimate_p_mispredict(fn, cond, source_line);

    double score_branch, score_select;
    if (result_loop_carried) {
        // El cmov queda en la recurrencia de loop CADA iteracion (camino
        // critico); el branch paga el stall de misprediction cada iteracion.
        // Para ramas triviales esto es CMOV_LAT vs P*penalty (cruce ~0.13).
        score_select = kCmovLatency + 0.25 * (cost_true + cost_false);
        score_branch = p_mis * kMispredictPenalty;
    } else {
        // Sin recurrencia: el select especula AMBAS ramas; el branch ejecuta
        // solo la tomada y paga el fallo de prediccion.
        score_select = cost_true + cost_false + kCmovLatency;
        const double avg_body = 0.5 * (cost_true + cost_false);
        score_branch = p_mis * kMispredictPenalty + avg_body;
    }
    return score_select < score_branch;
}

int load_branch_profile(const char *path) {
    if (!path) return 0;
    std::FILE *f = std::fopen(path, "r");
    if (!f) return 0;
    int n = 0;
    unsigned line = 0;
    unsigned long long taken = 0, nt = 0;
    while (std::fscanf(f, "%u %llu %llu", &line, &taken, &nt) == 3) {
        const unsigned long long total = taken + nt;
        if (line == 0 || total == 0) continue;
        const double mn = static_cast<double>(std::min(taken, nt));
        g_branch_profile[line] = mn / static_cast<double>(total);
        ++n;
    }
    std::fclose(f);
    return n;
}

} // namespace ir
