/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/build_requirements.h
 * @brief Ensamblador del SNAPSHOT: @c build_value_requirements(fn) corre TODOS
 *        los adaptadores sobre una funcion real y produce el @c ValueRequirements
 *        por valor.
 *
 * Este es el punto donde el modelo deja de ser un conjunto de piezas y pasa a
 * ALIMENTARSE DE CODIGO REAL.  Arquitectura del ensamblaje:
 *
 *                         IrFunction (codigo real)
 *                                  |
 *          +-----------------------+------------------------+
 *          |                       |                        |
 *      compute_liveness      compute_loop_facts      compute_profile_facts
 *      (Tipo A)              (Tipo A)                 (Tipo B, opcional)
 *          |                       |                        |
 *          |     +-----------------+----------+-------------+------+
 *          |     |                 |          |             |      |
 *          v     v                 v          v             v      v
 *      +-------------+  +--------+ +--------+ +--------+ +---------+
 *      | Liveness    |  | Type   | | Const  | | Loop   | | Profile |   <- adaptadores
 *      | Adapter     |  | Adapter| | Adapter| | Adapter| | Adapter |   (solo traducen)
 *      +------+------+  +---+----+ +---+----+ +---+----+ +----+----+
 *   crosses_call|      cls/width| remat|    loop_depth| exec_weight|
 *          +----+-----------+-------+--------+---------+-----+
 *                                  v
 *                    ValueRequirements[]  (SNAPSHOT inmutable, 1 por valor)
 *                                  |
 *                   +--------------+---------------+
 *                   v                              v
 *          audit_requirements               Constraints -> Objective -> Decision
 *        (¿valores imposibles?)              (el resto de la piramide)
 *
 * SNAPSHOT INMUTABLE: se computan los Facts UNA vez, los adaptadores rellenan
 * cada @c ValueRequirements y el resultado es una FOTOGRAFIA congelada del
 * programa.  El resto del compilador (allocator/scheduler/...) trabaja sobre esa
 * foto en vez de recalcular Liveness/Loop por su cuenta.
 *
 * AUDITOR: una vez existe el snapshot, el compilador puede AUDITARSE a si mismo
 * (¿hay valores imposibles?, ¿un F64 acabo como GP?, ¿un fixed_reg incompatible?,
 * ¿una lane que no soporta el ancho?) via @c requirements_satisfiable sobre cada
 * valor real -- ver @c audit_requirements.
 *
 * Fase 0.25: ADITIVO.  Los adaptadores usados hoy: Type, Const, Liveness, Loop y
 * (si hay perfil) Profile.  @c is_gc se toma directo del flag del IR (un futuro
 * GcAdapter/AliasFacts lo formalizara); @c address_taken queda diferido.
 */

#ifndef VESTA_CODEGEN_RBANK_BUILD_REQUIREMENTS_H
#define VESTA_CODEGEN_RBANK_BUILD_REQUIREMENTS_H

#include "analysis/derived/profile_facts.h"
#include "analysis/facts/loop_facts.h"
#include "ir/liveness.h"
#include "ir/ssa_ir.h"
#include "codegen/rbank/adapters/const_adapter.h"
#include "codegen/rbank/adapters/liveness_adapter.h"
#include "codegen/rbank/adapters/loop_adapter.h"
#include "codegen/rbank/adapters/profile_adapter.h"
#include "codegen/rbank/adapters/type_adapter.h"
#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/value_requirements.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace jit {
namespace rbank {

/**
 * @brief Ensambla el snapshot de @c ValueRequirements de una funcion real.
 * @param fn    funcion SSA.
 * @param prof  perfil de branches (opcional); si es @c nullptr o vacio, el
 *              @c execution_weight queda 0 -> el contexto usa el estatico.
 * @return      un @c ValueRequirements por cada valor SSA con def/param/interval.
 */
inline std::vector<ValueRequirements> build_value_requirements(
    const ir::IrFunction &fn,
    const analysis::BranchProfile *prof = nullptr) {

    // Facts (una vez).
    ir::LivenessResult live = ir::compute_liveness(fn);
    std::vector<uint32_t> calls = collect_call_positions(fn, live);
    analysis::LoopFacts loops = analysis::compute_loop_facts(fn);
    analysis::ProfileFacts pf;
    if (prof && !prof->empty())
        pf = analysis::compute_profile_facts(fn, loops, *prof);

    // value_id -> bloque de definicion (params se definen en la entrada).
    std::unordered_map<ir::IrValueId, ir::IrBlockId> def_block;
    for (ir::IrValueId p : fn.params) def_block[p] = 0;
    for (size_t b = 0; b < fn.blocks.size(); ++b)
        for (const ir::IrInstr &ins : fn.blocks[b].instrs)
            if (ins.dst != ir::IR_NO_VALUE)
                def_block[ins.dst] = static_cast<ir::IrBlockId>(b);

    // value_id -> intervalo de vida.
    std::unordered_map<ir::IrValueId, const ir::LiveInterval *> iv;
    for (const ir::LiveInterval &I : live.intervals) iv[I.id] = &I;

    std::vector<ValueRequirements> out;
    out.reserve(fn.values.size());
    for (const ir::IrValue &v : fn.values) {
        if (v.type == ir::IrType::VOID) continue;
        const bool has_def = def_block.count(v.id) != 0;
        const bool has_iv = iv.count(v.id) != 0;
        if (!has_def && !has_iv) continue; // valor no materializado

        ValueRequirements r;
        r.value_id = v.id;
        populate_type_requirements(r, v.type);   // cls + width
        populate_const_requirements(r, v);        // rematerializable
        // is_gc: flag directo del IR (o tipo HANDLE).  Futuro: GcAdapter/AliasFacts.
        r.is_gc = v.is_gc_object || v.type == ir::IrType::HANDLE;

        auto it = iv.find(v.id);
        if (it != iv.end())
            populate_liveness_requirements(r, *it->second, calls); // crosses_call

        auto db = def_block.find(v.id);
        if (db != def_block.end()) {
            populate_loop_requirements(r, loops, db->second);       // loop_depth
            if (pf.has_profile)
                populate_profile_requirements(r, pf, db->second);   // execution_weight
        }
        out.push_back(r);
    }
    return out;
}

/**
 * @struct RequirementIssue
 * @brief Una INCOHERENCIA detectada por el auditor (DATO, no mensaje).
 */
struct RequirementIssue {
    uint32_t    value_id = 0;
    UnsatReason reason   = UnsatReason::OK; ///< por que el valor no es realizable.
};

/**
 * @brief AUDITA el snapshot contra un banco: reporta los valores IMPOSIBLES
 *        (que el hardware no puede alojar).  El compilador auditandose a si mismo.
 * @param reqs      snapshot de @c build_value_requirements.
 * @param bank      banco fisico del target.
 * @param vec_reduction_active  si el path de reduccion vectorial esta activo.
 * @return          lista de incoherencias (vacia = todo realizable).
 */
inline std::vector<RequirementIssue> audit_requirements(
    const std::vector<ValueRequirements> &reqs,
    const PhysicalRegisterBank &bank, bool vec_reduction_active = false) {

    std::vector<RequirementIssue> issues;
    for (const ValueRequirements &r : reqs) {
        SatisfiabilityReport sat =
            requirements_satisfiable(r, bank, vec_reduction_active);
        if (!sat.ok) issues.push_back({r.value_id, sat.reason});
    }
    return issues;
}

} // namespace rbank
} // namespace jit

#endif // VESTA_CODEGEN_RBANK_BUILD_REQUIREMENTS_H
