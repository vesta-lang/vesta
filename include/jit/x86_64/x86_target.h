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
 * @file jit/x86_64/x86_target.h
 * @brief @ref CodegenTarget para x86-64 / x86-32: envuelve el selector
 *        (@c vreg_select HOST_LEAF), el rewrite (@c rewrite_to_physical) y el
 *        encoder (@c X86Encoder) existentes.  No cambia el codegen x86; solo lo
 *        expone tras la interfaz comun para el pipeline multi-ISA.
 */

#ifndef VESTA_JIT_X86_64_X86_TARGET_H
#define VESTA_JIT_X86_64_X86_TARGET_H

#include "jit/codegen_target.h"
#include "jit/peephole.h"
#include "jit/regalloc_rewrite.h"
#include "jit/vreg_select.h"
#include "jit/x86_encoder.h"

namespace jit {

/// @c CodegenTarget x86 (config del AOT: ABI/float/32-bit/resolvers).
class X86Target final : public CodegenTarget {
  public:
    X86Target(const CallResolver &resolve_call, const VregEntries &ent,
              const CallResolver &resolve_native,
              const CallResolver &resolve_symbol, bool pic, bool target_sysv,
              bool mode32, FloatIsa fisa, bool emit_line_map,
              bool reserve_vec_acc = true)
        : resolve_call_(resolve_call), ent_(ent),
          resolve_native_(resolve_native), resolve_symbol_(resolve_symbol),
          pic_(pic), sysv_(target_sysv), mode32_(mode32), fisa_(fisa),
          emit_line_map_(emit_line_map), reserve_vec_acc_(reserve_vec_acc) {}

    const TargetRegInfo &reg_info() const override {
        // Reserva VEC_ACC demand-driven: XMM10-13 solo se reservan en funciones
        // que usan el path vectorial; las escalares obtienen 14 lanes FP.
        return mode32_ ? target_x86_32()
                       : target_x86_64_abi(sysv_, reserve_vec_acc_);
    }

    sched::EffIsa sched_isa() const override { return sched::EffIsa::X86; }

    bool select(const ir::IrFunction &fn, MFunction &out) const override {
        return vreg_select(fn, out, AbiKind::HOST_LEAF, resolve_call_, ent_,
                           resolve_native_, resolve_symbol_, pic_, sysv_,
                           mode32_, fisa_, emit_line_map_);
    }

    MFunction rewrite(const MFunction &vf, const codegen::RegAlloc &ra,
                      const IntervalResult &ivs) const override {
        return rewrite_to_physical(vf, ra, reg_info(), AbiKind::HOST_LEAF,
                                   &ivs);
    }

    int encode(MFunction &pf, std::vector<uint8_t> &out) const override {
        X86Encoder enc;
        enc.set_mode32(mode32_);
        enc.set_vx_scalar(fisa_ == FloatIsa::AVX || fisa_ == FloatIsa::AVX512F);
        return enc.encode(pf, out);
    }

    void peephole(MFunction &pf) const override { peephole_physical(pf); }

  private:
    const CallResolver &resolve_call_;
    const VregEntries &ent_;
    const CallResolver &resolve_native_;
    const CallResolver &resolve_symbol_;
    bool pic_;
    bool sysv_;
    bool mode32_;
    FloatIsa fisa_;
    bool emit_line_map_;
    bool reserve_vec_acc_; ///< reservar XMM10-13 para VEC_ACC (demand-driven).
};

} // namespace jit

#endif // VESTA_JIT_X86_64_X86_TARGET_H
