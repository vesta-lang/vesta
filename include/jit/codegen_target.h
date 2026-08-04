/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file jit/codegen_target.h
 * @brief Interfaz de un TARGET de codegen (desacople multi-ISA del pipeline
 *        vreg).  El orquestador (@c vreg_compile_native_target) es arch-neutral:
 *        los tres pasos ISA-especificos -- SELECCION (IR->MachineIR), REWRITE
 *        (vreg->fisico + prologo/epilogo/spills) y ENCODE (MachineIR->bytes) --
 *        viven detras de esta interfaz.  El resto del pipeline (build_intervals,
 *        linear_scan, ssa_coalesce, scheduler, traduccion de relocs) es comun.
 *
 * Un target se CONSTRUYE con su configuracion (ABI, resolvers, float ISA...) y
 * luego el pipeline lo usa sin conocer la ISA.  Implementaciones: @c X86Target
 * (envuelve el selector/rewrite/encoder x86 existentes) y @c Arm64Target.
 */

#ifndef VESTA_JIT_CODEGEN_TARGET_H
#define VESTA_JIT_CODEGEN_TARGET_H

#include "jit/interval.h"        // IntervalResult
#include "codegen/regalloc.h"
#include "jit/machine_ir.h"      // MFunction
#include "jit/sched/machine_effects.h" // sched::EffIsa
#include "jit/target_reginfo.h"  // TargetRegInfo

#include <cstdint>
#include <vector>

namespace ir {
struct IrFunction;
}

namespace jit {

/**
 * @brief Un target de codegen para el pipeline vreg.  Encapsula las 3 etapas
 *        dependientes de la ISA; el pipeline llama a los metodos en orden.
 */
class CodegenTarget {
  public:
    virtual ~CodegenTarget() = default;

    /// Descripcion de registros para el regalloc (allocatable/scratch/ABI).
    virtual const TargetRegInfo &reg_info() const = 0;

    /// ISA para los efectos del scheduler (elige la DB de instrucciones).
    virtual sched::EffIsa sched_isa() const = 0;

    /**
     * @brief SELECCION: baja @p fn (SSA IR) a MachineIR de vregs en @p out.
     * @return false si @p fn usa un op fuera del subset -> fallback.
     */
    virtual bool select(const ir::IrFunction &fn, MFunction &out) const = 0;

    /**
     * @brief REWRITE: convierte el MachineIR de vregs @p vf en fisico usando la
     *        asignacion @p ra, insertando prologo/epilogo y spills de la ABI.
     */
    virtual MFunction rewrite(const MFunction &vf, const codegen::RegAlloc &ra,
                              const IntervalResult &ivs) const = 0;

    /**
     * @brief ENCODE: emite los bytes maquina de @p pf (MachineIR fisico).  @p pf
     *        NO es const: el encoder rellena line_map / relocs / stackmaps /
     *        label_offsets que el orquestador consume despues.
     * @return numero de bytes (0 = fallo).
     */
    virtual int encode(MFunction &pf, std::vector<uint8_t> &out) const = 0;

    /// Limpieza post-rewrite (borrar self-moves del coalescing).  Por defecto
    /// generica (elimina @c MOV con dst==src fisico); un target puede afinar.
    virtual void peephole(MFunction &pf) const;
};

} // namespace jit

#endif // VESTA_JIT_CODEGEN_TARGET_H
