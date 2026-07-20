/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/memory/memory_access.h
 * @brief Vocabulario UNICO de "acceso a memoria de una instruccion IR": que lee
 *        y que escribe, y a que localizacion (AbstractLoc, via el resolvedor
 *        points-to compartido).  Es la UNA SOLA VERDAD que consumen TODOS los
 *        pases sensibles a memoria (DSE, LICM, scheduler semantico,
 *        EffectAnalysis, ...): ninguno reimplementa el switch LOAD/STORE/
 *        GETFIELD/ARRAY_x/MEMCPY ni su propio access_bytes.  Asi todos hablan
 *        el mismo idioma y una op nueva se modela en UN sitio.
 */
#ifndef ANALYSIS_MEMORY_MEMORY_ACCESS_H
#define ANALYSIS_MEMORY_MEMORY_ACCESS_H

#include "analysis/effects/effects.h" // AbstractLoc
#include "analysis/memory/points_to.h"

#include <cstdint>

namespace ir {
struct IrInstr;
enum class IrType : uint8_t; // fwd; el valor real viene de ssa_ir.h
} // namespace ir

namespace analysis {

/// Bytes accedidos por un LOAD/STORE segun su IrType (1/2/4/8).  UNA sola
/// definicion para todo el compilador (antes duplicada en DSE/LICM/effects/
/// scheduler).  0 nunca: el default conservador es 8.
int32_t memory_access_size(ir::IrType t);

/// Acceso a memoria de UNA instruccion: que lee, que escribe, y donde.
/// - LOAD/GETFIELD/ARRAY_LOAD/ARRAY_LEN -> is_load, read_loc.
/// - STORE/SETFIELD/ARRAY_STORE         -> is_store, write_loc.
/// - MEMCPY                             -> is_load+is_store, read_loc=src,
///                                          write_loc=dst (NO es opaco: su
///                                          footprint se conoce).
/// - Ops que tocan memoria de forma NO localizable (str-ops alloc-side, gc,
///   atomicos, raw_asm, calls) -> opaque=true (el consumidor decide: barrera).
/// - El resto (aritmetica pura, control, ...) -> touches=false.
struct MemoryAccess {
    bool touches = false;  ///< accede a memoria (localizable u opaca)
    bool is_load = false;
    bool is_store = false;
    bool opaque = false;   ///< toca memoria pero NO se puede localizar (top)
    effects::AbstractLoc read_loc;  ///< localizacion leida (si is_load y !opaque)
    effects::AbstractLoc write_loc; ///< localizacion escrita (si is_store y !opaque)
};

/// Calcula el @c MemoryAccess de @p ins usando la tabla points-to @p pt.  El
/// consumidor NO construye pt: lo recibe (Regla 1 -- base de hechos compartida).
MemoryAccess memory_access(const ir::IrInstr &ins, const PointsTo &pt);

} // namespace analysis

#endif // ANALYSIS_MEMORY_MEMORY_ACCESS_H
