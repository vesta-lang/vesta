/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/derived/profile_facts.cpp
 * @brief Implementacion de @c compute_profile_facts (ver profile_facts.h).
 *
 * v1 APROXIMADO: trip-count por la heuristica "rama comun continua, rama rara
 * sale" (max/min).  El camino correcto (CFGProfile -> LoopProfile) llegara
 * cuando un consumidor exija precision con multiples exits / switches /
 * irreducibles.
 */

#include "analysis/derived/profile_facts.h"

namespace analysis {

using ir::IrBlockId;
using ir::IrFunction;

namespace {

/// Cota superior del trip-count para evitar pesos absurdos con ramas 0.
constexpr double kMaxTrip = 1.0e6;

/** @brief Linea fuente del branch condicional de la cabecera @p header (0 si no). */
uint32_t header_branch_line(const IrFunction &fn, IrBlockId header) {
    if (static_cast<size_t>(header) >= fn.blocks.size()) return 0;
    for (const ir::IrInstr &ins : fn.blocks[header].instrs) {
        // Un branch condicional tiene false_block (dos salidas) -> es la condicion.
        if (ins.false_block != ir::IR_NO_BLOCK && ins.source_line != 0)
            return ins.source_line;
    }
    return 0;
}

/**
 * @brief Trip-count APROXIMADO de un branch de cabecera (v1): la rama COMUN es
 *        continuar (mayor conteo), la RARA es salir (menor).  trip ~ max/min.
 *        Ignora multiples exits / switches (los cubrira CFGProfile/LoopProfile).
 * @return 0 si no hay perfil de esa linea.
 */
double trip_from_counts(uint64_t taken, uint64_t not_taken) {
    const uint64_t hi = taken > not_taken ? taken : not_taken;
    const uint64_t lo = taken > not_taken ? not_taken : taken;
    if (hi == 0) return 0.0;            // sin muestras
    if (lo == 0) return kMaxTrip;       // rama de salida nunca vista -> muchisimas iters
    double t = static_cast<double>(hi) / static_cast<double>(lo);
    return t > kMaxTrip ? kMaxTrip : t;
}

} // namespace

ProfileFacts compute_profile_facts(const IrFunction &fn, const LoopFacts &loops,
                                   const BranchProfile &prof) {
    const size_t N = fn.blocks.size();
    ProfileFacts pf;
    pf.block_weight.assign(N, 0.0);
    pf.trip_count.assign(loops.loop_count, 0.0);
    if (prof.empty()) {
        pf.has_profile = false;
        return pf; // sin perfil -> todo 0 (el consumidor usa el estatico).
    }
    pf.has_profile = true;

    // Trip-count por bucle desde el branch de su cabecera.
    for (uint32_t li = 0; li < loops.loop_count; ++li) {
        const IrBlockId h = static_cast<IrBlockId>(loops.header_block_of(li));
        const uint32_t line = header_branch_line(fn, h);
        uint64_t t = 0, nt = 0;
        if (line != 0 && prof.counts(line, t, nt))
            pf.trip_count[li] = trip_from_counts(t, nt);
    }

    // Peso por bloque = producto de trip-counts de los bucles que lo contienen
    // (cadena de anidamiento desde el mas interno via parent_loop).  Fuera de
    // bucle = 1 (una vez por llamada).  Todos los trips desconocidos = 0.
    for (size_t b = 0; b < N; ++b) {
        if (!loops.inside(static_cast<IrBlockId>(b))) {
            pf.block_weight[b] = 1.0;
            continue;
        }
        double w = 1.0;
        bool known = false;
        for (uint32_t L = loops.innermost(static_cast<IrBlockId>(b));
             L != LoopFacts::NO_LOOP; L = loops.parent_of(L)) {
            const double tc = pf.trip_of(L);
            if (tc > 0.0) { w *= tc; known = true; }
        }
        pf.block_weight[b] = known ? w : 0.0;
    }
    return pf;
}

std::vector<FactIssue> validate(const ProfileFacts &f) {
    std::vector<FactIssue> issues;
    for (size_t b = 0; b < f.block_weight.size(); ++b)
        if (f.block_weight[b] < 0.0)
            issues.push_back({FactCheck::PROFILE_NEGATIVE_WEIGHT, b, 0});
    for (size_t L = 0; L < f.trip_count.size(); ++L)
        if (f.trip_count[L] < 0.0)
            issues.push_back({FactCheck::PROFILE_NEGATIVE_TRIP, L, 0});
    return issues;
}

} // namespace analysis
