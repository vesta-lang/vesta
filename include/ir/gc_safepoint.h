/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ir/gc_safepoint.h
 * @brief Pase de IR COMPARTIDO: conjunto de raices GC por safepoint.
 *
 * = Modelo (LLVM gc.statepoint) =
 *
 * La deteccion de RAICES GC en cada safepoint es SEMANTICA y vive a nivel
 * IR -- es INDEPENDIENTE del backend.  El mismo conjunto logico de valores
 * SSA vivos de tipo GC sirve para el interprete (PC de bytecode), el JIT
 * (offset nativo) y el AOT (direccion).  Se calcula UNA sola vez aqui.
 *
 * La UBICACION FISICA de cada raiz (que registro / slot / offset) es lo
 * unico que depende del backend: cada backend materializa este conjunto a
 * sus ubicaciones fisicas con su propio regalloc.
 *
 *   Interp  : raiz SSA -> registro VM (R0..R15) o slot de spill (VSMP).
 *   JIT/AOT : raiz SSA -> registro nativo o offset RBP (stackmap nativo).
 *
 * Este header es la UNICA fuente de verdad de "que es raiz en un safepoint".
 * El interprete lo consume hoy; el JIT (D.2, que hoy computa sus raices en
 * el selector) y el AOT deben adoptarlo para no divergir sobre el conjunto
 * de raices.
 */

#ifndef VESTA_IR_GC_SAFEPOINT_H
#define VESTA_IR_GC_SAFEPOINT_H

#include <algorithm>
#include <cstdint>
#include <vector>

#include "ir/ssa_ir.h"
#include "ir/liveness.h"

namespace ir {

/**
 * @brief True si @p op es un SAFEPOINT: un IR op cuyo bytecode/codigo nativo
 *        puede disparar el recolector de basura (alocacion directa).
 *
 * Cubre las alocaciones directas que llaman a @c gc_heap.alloc():
 * NEWOBJ / NEWOBJS / GC_ALLOC / GC_ALLOCP.  Los CALLs (cuyo callee puede
 * alocar) son safepoints tambien, pero alli el GC corre en el frame del
 * callee y el frame del caller se identifica por la direccion de retorno
 * (manejo distinto por backend); no se incluyen en esta version del pase.
 */
inline bool is_gc_safepoint(IrOp op) {
    switch (op) {
    case IrOp::NEWOBJ:
    case IrOp::NEWOBJS:
    case IrOp::GC_ALLOC:
    case IrOp::GC_ALLOCP:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Conjunto de raices GC vivas en un safepoint (a nivel IR, sin
 *        ubicacion fisica).
 */
struct SafepointRoots {
    uint32_t lin_pos = 0;           ///< posicion lineal del safepoint (liveness)
    std::vector<IrValueId> roots;   ///< SSA values vivos con is_gc_object
};

/**
 * @brief Devuelve el conjunto de raices GC vivas en la posicion lineal
 *        @p pos del safepoint, EXCLUYENDO @p def_dst (nace tras el GC).
 *
 * Un valor SSA es raiz en @p pos si:
 *   - su intervalo de liveness cubre @p pos (@c def <= pos < end), y
 *   - su tipo es @c is_gc_object (referencia gestionada por el GC).
 *
 * El resultado esta deduplicado por id y ordenado ascendentemente.  Es la
 * parte COMPARTIDA (semantica): cada backend lo materializa a ubicaciones
 * fisicas con su propio regalloc.
 *
 * @param fn       funcion SSA.
 * @param liveness intervalos de vida calculados para @p fn.
 * @param pos      posicion lineal del safepoint.
 * @param def_dst  valor SSA definido por el safepoint (a excluir), o
 *                 @c IR_NO_VALUE.
 *
 * Inline (header-only) para estar disponible en cualquier TU/target que
 * enlace @c ir_emitter sin requerir un .cpp dedicado en cada lista de
 * fuentes (el JIT/AOT que lo adopten lo obtienen igual con solo incluir).
 */
inline std::vector<IrValueId>
safepoint_gc_roots(const IrFunction &fn, const LivenessResult &liveness,
                   uint32_t pos, IrValueId def_dst) {
    std::vector<IrValueId> roots;
    roots.reserve(8);
    // Parte SEMANTICA compartida: valor vivo en pos (def <= pos < end) y
    // is_gc_object.  Excluye el dst del safepoint (nace tras el GC).
    for (const auto &iv : liveness.intervals) {
        if (iv.id == def_dst) continue;
        if (!(iv.def <= pos && pos < iv.end)) continue;
        if (static_cast<size_t>(iv.id) >= fn.values.size()) continue;
        if (!fn.values[iv.id].is_gc_object) continue;
        roots.push_back(iv.id);
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

} // namespace ir

#endif // VESTA_IR_GC_SAFEPOINT_H
