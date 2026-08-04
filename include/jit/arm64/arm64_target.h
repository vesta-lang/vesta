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
 * @file jit/arm64/arm64_target.h
 * @brief @ref CodegenTarget para AArch64: enchufa arm64 al pipeline vreg
 *        (MachineIR + regalloc generico + scheduler), sustituyendo al template
 *        slot-por-valor.  select (IR->MachineIR AAPCS64) + rewrite (vreg->fisico
 *        + prologo/epilogo/spills) + encode (MachineIR->AArch64 via Keystone).
 */

#ifndef VESTA_JIT_ARM64_ARM64_TARGET_H
#define VESTA_JIT_ARM64_ARM64_TARGET_H

#include "jit/codegen_target.h"

namespace jit {

/// @c CodegenTarget AArch64.  Por ahora cubre el subset entero (const/mov/ALU/
/// cmp/branches/ret/params/call); float y mas ops se anaden por incrementos.
class Arm64Target final : public CodegenTarget {
  public:
    const TargetRegInfo &reg_info() const override { return target_arm64(); }
    sched::EffIsa sched_isa() const override { return sched::EffIsa::ARM64; }
    bool select(const ir::IrFunction &fn, MFunction &out) const override;
    MFunction rewrite(const MFunction &vf, const codegen::RegAlloc &ra,
                      const IntervalResult &ivs) const override;
    int encode(MFunction &pf, std::vector<uint8_t> &out) const override;
};

} // namespace jit

#endif // VESTA_JIT_ARM64_ARM64_TARGET_H
