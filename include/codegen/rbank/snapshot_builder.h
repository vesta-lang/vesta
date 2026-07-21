/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/snapshot_builder.h
 * @brief SnapshotBuilder: el ALGORITMO que construye la @c FunctionSnapshot.
 *
 * Separacion DATO vs ALGORITMO: el snapshot (function_snapshot.h) es un DATO; su
 * construccion es un ALGORITMO y vive aqui.  Esto mantiene la API limpia y,
 * sobre todo, hace que el crecimiento futuro NO rompa el punto de entrada:
 * cuando lleguen DomFacts/AliasFacts/EscapeFacts/MemoryFacts, se anaden como
 * @c Fact nuevos + su rama en @c build; los consumidores que piden
 * @c Fact::All no cambian.
 *
 * Se puede pedir un SUBCONJUNTO de Facts (no siempre hace falta todo), y el
 * builder RESUELVE LAS DEPENDENCIAS automaticamente:
 *
 *      builder.enable(Fact::Values).build(fn)
 *                    |
 *          resolve() : Values -> {Liveness, Loops};  Profile -> {Loops}
 *                    |
 *      +-------------+-------------+-------------+
 *      v             v             v             v
 *   Liveness      Loops        Profile        Values
 *  (si pedido    (si pedido   (si pedido +   (corre los
 *   o dep)        o dep)       hay perfil)    adaptadores)
 *                    |
 *              FunctionSnapshot
 *
 * Ejemplo:
 * @code
 *   SnapshotBuilder b;
 *   b.enable(Fact::Loops).enable(Fact::Values).with_profile(prof);
 *   FunctionSnapshot s = b.build(fn);   // Liveness se activa por dependencia
 * @endcode
 */

#ifndef VESTA_CODEGEN_RBANK_SNAPSHOT_BUILDER_H
#define VESTA_CODEGEN_RBANK_SNAPSHOT_BUILDER_H

#include "analysis/derived/profile_facts.h"
#include "analysis/facts/loop_facts.h"
#include "codegen/rbank/build_requirements.h"
#include "codegen/rbank/function_snapshot.h"
#include "ir/liveness.h"
#include "ir/ssa_ir.h"

#include <cstdint>
#include <vector>

namespace jit {
namespace rbank {

// @c Fact vive en function_snapshot.h (es de la interfaz del snapshot).

/**
 * @struct SnapshotBuilder
 * @brief Construccion EAGER de una @c FunctionSnapshot con los Facts pedidos
 *        (+ dependencias).  Fuerza el query system lazy del snapshot para los
 *        hechos seleccionados -- util para precomputar un subconjunto (p.ej.
 *        para serializar o "calentar" la foto).  Sin builder, el snapshot ya es
 *        lazy: pedir @c snapshot.value_reqs() arrastra sus dependencias solo.
 */
struct SnapshotBuilder {
    uint32_t                       enabled = 0;       ///< mascara de Facts pedidos.
    const analysis::BranchProfile *prof    = nullptr; ///< perfil (opcional).

    SnapshotBuilder &enable(Fact f) noexcept {
        enabled |= static_cast<uint32_t>(f);
        return *this;
    }
    SnapshotBuilder &with_profile(const analysis::BranchProfile &p) noexcept {
        prof = &p;
        return enable(Fact::Profile);
    }
    static bool has(uint32_t e, Fact f) noexcept {
        return (e & static_cast<uint32_t>(f)) != 0;
    }

    /** @brief Cierra las DEPENDENCIAS: Values->{Liveness,Loops}; Profile->Loops. */
    static uint32_t resolve(uint32_t e) noexcept {
        if (e & static_cast<uint32_t>(Fact::Values))
            e |= static_cast<uint32_t>(Fact::Liveness) |
                 static_cast<uint32_t>(Fact::Loops);
        if (e & static_cast<uint32_t>(Fact::Profile))
            e |= static_cast<uint32_t>(Fact::Loops);
        return e;
    }

    /** @brief Construye la foto forzando (eager) los Facts resueltos. */
    FunctionSnapshot build(const ir::IrFunction &fn) const {
        const uint32_t e = resolve(enabled);
        FunctionSnapshot s;
        s.fn   = &fn;
        s.prof = prof;
        // Forzar via los accessors lazy (computan + cachean + resuelven deps).
        if (has(e, Fact::Liveness)) (void)s.liveness();
        if (has(e, Fact::Loops))    (void)s.loop_facts();
        if (has(e, Fact::Profile))  (void)s.profile_facts();
        if (has(e, Fact::Values))   (void)s.value_reqs();
        return s;
    }
};

/**
 * @brief Conveniencia: fotografia COMPLETA de @p fn (todos los Facts).
 * @param prof  perfil de branches (opcional).
 */
inline FunctionSnapshot build_snapshot(
    const ir::IrFunction &fn,
    const analysis::BranchProfile *prof = nullptr) {
    SnapshotBuilder b;
    b.enable(Fact::All);
    if (prof) b.with_profile(*prof);
    return b.build(fn);
}

} // namespace rbank
} // namespace jit

#endif // VESTA_CODEGEN_RBANK_SNAPSHOT_BUILDER_H
