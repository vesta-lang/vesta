/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/vm_shadow.h
 * @brief MODO SOMBRA del camino del INTERPRETE: correr @c codegen::rbank EN
 *        PARALELO a @c ir::allocate_regs, comparar, y NO consumir su resultado.
 *
 * POR QUE ASI Y NO CAMBIANDO EL CONSUMIDOR DIRECTAMENTE.  El interprete es el
 * ORACULO de @c diff_harness: es contra el que se valida que el JIT y el AOT
 * generan lo correcto.  Cambiar su asignador de golpe reescribiria el `.vel` de
 * los 424 programas del corpus A LA VEZ, y perderiamos la referencia justo
 * cuando mas falta hace -- no habria forma de distinguir "el modelo decide
 * distinto" de "el modelo decide MAL".
 *
 * En sombra la pregunta se responde antes de arriesgar nada: ¿el modelo ve el
 * mismo problema?  ¿decide igual o mejor?  Es exactamente como se valido rbank
 * contra @c linear_scan en el path vreg antes de jubilarlo (ver
 * @c codegen/rbank/shadow.h, "la primera pregunta EXTERNA: ¿el modelo describe
 * correctamente PROGRAMAS REALES?").
 *
 * COSTE.  Cerrado por defecto: sin @c VESTA_VM_SHADOW=1 no se construye el
 * problema ni se colorea nada -- una lectura de un bool estatico por funcion
 * compilada.  Con la puerta abierta se paga una asignacion extra por funcion,
 * que es el precio de la respuesta.
 *
 * QUE SE COMPARA (y que NO).  Se comparan METRICAS DE CALIDAD: cuantos valores
 * hay, cuantos se derraman y cual es el pico de presion.  NO se compara la
 * asignacion registro a registro: dos asignaciones distintas pueden ser ambas
 * correctas, y exigir igualdad literal convertiria una mejora en un "fallo".
 * Lo que importa es que el modelo no derrame MAS.
 */

#ifndef VESTA_CODEGEN_VM_SHADOW_H
#define VESTA_CODEGEN_VM_SHADOW_H

#include "codegen/rbank/allocate.h"
#include "codegen/rbank/allowed_lanes.h"
#include "codegen/rbank/coloring.h"
#include "codegen/rbank/optimization_context.h"
#include "codegen/rbank/physical_bank.h"
#include "codegen/rbank/smart_spill.h"
#include "codegen/vm_problem.h"
#include "codegen/vm_target.h"
#include "ir/liveness.h"
#include "ir/regalloc.h"
#include "ir/ssa_ir.h"
#include "jit/backend_caps.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace codegen {

/**
 * @struct VmShadowStats
 * @brief Comparacion agregada entre el asignador del emisor y el modelo.
 */
struct VmShadowStats {
    uint32_t functions = 0;      ///< funciones comparadas.
    uint64_t values = 0;         ///< valores vivos considerados.
    uint64_t spills_emitter = 0; ///< derrames de ir::allocate_regs.
    uint64_t spills_model = 0;   ///< derrames del modelo (rbank).
    uint32_t fn_model_better = 0; ///< funciones donde el modelo derrama MENOS.
    uint32_t fn_model_worse = 0;  ///< funciones donde derrama MAS  <- lo grave.
    uint32_t fn_equal = 0;        ///< funciones con el mismo numero de derrames.
    /// Peor caso individual, para poder ir a mirarlo: nombre y diferencia.
    std::string worst_fn;
    int32_t worst_delta = 0;

    void add_fn(const std::string &name, uint32_t v, uint32_t se, uint32_t sm) {
        ++functions;
        values += v;
        spills_emitter += se;
        spills_model += sm;
        const int32_t d = static_cast<int32_t>(sm) - static_cast<int32_t>(se);
        if (d < 0) ++fn_model_better;
        else if (d > 0) ++fn_model_worse;
        else ++fn_equal;
        if (d > worst_delta) { worst_delta = d; worst_fn = name; }
    }
};

/** @brief ¿Esta abierta la puerta del modo sombra?  @c VESTA_VM_SHADOW=1. */
inline bool vm_shadow_enabled() {
    static const bool on = [] {
        const char *e = std::getenv("VESTA_VM_SHADOW");
        return e && e[0] && e[0] != '0';
    }();
    return on;
}

/** @brief Acumulador global (una sola compilacion puede tener muchas funciones). */
inline VmShadowStats &vm_shadow_agg() {
    static VmShadowStats s;
    return s;
}
inline std::mutex &vm_shadow_mutex() {
    static std::mutex m;
    return m;
}

/** @brief Vuelca la comparacion a stderr.  Se registra en @c atexit. */
inline void print_vm_shadow_summary() {
    VmShadowStats s;
    {
        std::lock_guard<std::mutex> lk(vm_shadow_mutex());
        s = vm_shadow_agg();
    }
    if (s.functions == 0) return;
    std::fprintf(stderr,
                 "\n=== [vm-shadow] emisor (ir::allocate_regs) vs modelo (rbank) ===\n"
                 "  funciones .............. %u\n"
                 "  valores vivos .......... %llu\n"
                 "  derrames emisor ........ %llu\n"
                 "  derrames modelo ........ %llu\n"
                 "  por funcion: modelo MEJOR=%u  igual=%u  PEOR=%u\n",
                 s.functions, (unsigned long long)s.values,
                 (unsigned long long)s.spills_emitter,
                 (unsigned long long)s.spills_model, s.fn_model_better,
                 s.fn_equal, s.fn_model_worse);
    if (s.fn_model_worse)
        std::fprintf(stderr,
                     "  peor caso: %s (+%d derrames)  <- mirar ESTE primero\n",
                     s.worst_fn.c_str(), s.worst_delta);
    else
        std::fprintf(stderr, "  el modelo NO derrama de mas en ninguna funcion.\n");
}

/**
 * @brief Corre el modelo sobre @p fn y acumula la comparacion.  NO devuelve
 *        nada: su resultado no se consume, solo se mide.
 *
 * @param fn        funcion SSA.
 * @param live      vivacidad del IR (la misma que usa el emisor).
 * @param emitter   resultado del asignador actual, para comparar.
 * @param coalesce_remap  el MISMO remap que recibe ir::allocate_regs.  Sin el,
 *        la comparacion no seria entre dos asignadores sino entre DOS PROBLEMAS
 *        distintos: el emisor asigna sobre valores canonicos (congruencias de
 *        PHI ya fundidas) y el modelo veria mas valores vivos de los que hay.
 */
inline void vm_shadow_compare(const ir::IrFunction &fn,
                              const ir::LivenessResult &live,
                              const codegen::RegAlloc &emitter,
                              const std::vector<uint32_t> *coalesce_remap) {
    if (!vm_shadow_enabled()) return; // camino cerrado: coste ~0.
    static std::once_flag once;
    std::call_once(once, [] { std::atexit(print_vm_shadow_summary); });

    const rbank::AbstractProblem p =
        liveness_to_problem(fn, live, coalesce_remap);
    if (p.values.empty()) return;

    rbank::PhysicalRegisterBank bank =
        rbank::physical_bank_x86_64_from_reginfo(target_vm(),
                                                 jit::backend_caps_host());
    rbank::ConstraintSet cs;
    rbank::OptimizationContext ctx = rbank::make_context(bank, cs);
    const rbank::LaneAssignment la =
        rbank::color_smart_spill(p, ctx, /*vec_active=*/false);

    uint32_t spills_model = 0;
    for (const rbank::AbstractValue &v : p.values)
        if (la.lane_of(v.value_id) == rbank::kSpilled) ++spills_model;

    std::lock_guard<std::mutex> lk(vm_shadow_mutex());
    vm_shadow_agg().add_fn(fn.name, static_cast<uint32_t>(p.values.size()),
                           emitter.num_spill_slots, spills_model);
}

/** @brief ¿Asigna el modelo en vez de @c ir::allocate_regs?  @c VESTA_VM_RBANK. */
inline bool vm_rbank_enabled() {
    static const bool on = [] {
        const char *e = std::getenv("VESTA_VM_RBANK");
        return e && e[0] && e[0] != '0';
    }();
    return on;
}

/**
 * @brief Asigna los registros del camino del INTERPRETE con @c codegen::rbank
 *        -- el mismo allocator que ya usan el JIT y el AOT.
 *
 * La funcion se limita a construir el problema en el dominio del IR y delegar en
 * @c rbank_solve.  NO reimplementa la secuencia greedy/taxonomia/Recovery: si lo
 * hiciera, el interprete tendria su propia copia del allocator y volveriamos al
 * punto de partida con otro nombre.
 *
 * @param coalesce_remap  congruencias de PHI.  El adaptador las FUNDE al entrar
 *        (congruentes SON el mismo valor) y aqui se REPARTEN al salir: son la ida
 *        y la vuelta de la misma transformacion.
 */
inline codegen::RegAlloc
vm_allocate(const ir::IrFunction &fn, const ir::LivenessResult &live,
            const std::vector<uint32_t> *coalesce_remap) {
    const rbank::AbstractProblem p = liveness_to_problem(fn, live, coalesce_remap);
    const uint32_t n = static_cast<uint32_t>(fn.values.size());
    if (p.values.empty()) {
        codegen::RegAlloc empty;
        empty.assign.assign(n, codegen::RegAlloc::VAssign{});
        return empty;
    }
    const rbank::PhysicalRegisterBank bank =
        rbank::physical_bank_x86_64_from_reginfo(target_vm(), jit::backend_caps_host());
    codegen::RegAlloc ra =
        rbank::rbank_solve(p, n, bank, /*vec_active=*/false, /*next_use=*/nullptr,
                           /*trace=*/nullptr, /*recover=*/true);

    /* Repartir la asignacion del root a los demas miembros de su clase.  El
     * problema tenia UN valor por clase (fundido en el adaptador), asi que sin
     * esto los miembros no-root quedarian sin ubicacion. */
    if (coalesce_remap && !coalesce_remap->empty()) {
        const std::vector<uint32_t> &remap = *coalesce_remap;
        ra.assign.resize(n);
        for (uint32_t v = 0; v < n; ++v) {
            const uint32_t rt = (v < remap.size()) ? remap[v] : v;
            if (rt == v || rt >= ra.assign.size()) continue;
            ra.assign[v] = ra.assign[rt];
        }
    }
    return ra;
}

} // namespace codegen

#endif // VESTA_CODEGEN_VM_SHADOW_H
