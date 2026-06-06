/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/regalloc_rewrite.cpp
 * @brief Implementacion del rewrite vreg -> fisico (Phase D.7, commit 4a).
 *        Ver regalloc_rewrite.h y doc/REGALLOC.md.
 */

#include "jit/regalloc_rewrite.h"

namespace jit {

    namespace {

        /** @brief True si la ALU binaria @p op es conmutativa. */
        bool is_commutative(MOp op) noexcept {
            switch (op) {
                case MOp::ADD: case MOp::AND:
                case MOp::OR:  case MOp::XOR:
                case MOp::IMUL: return true;
                default:        return false;
            }
        }

        /** @brief True si @p op es una ALU binaria que reescribimos (4a). */
        bool is_bin_alu(MOp op) noexcept {
            switch (op) {
                case MOp::ADD: case MOp::SUB: case MOp::AND:
                case MOp::OR:  case MOp::XOR: case MOp::IMUL: return true;
                default:        return false;
            }
        }

        /**
         * @struct Lowerer
         * @brief Estado del rewrite de una funcion.
         */
        struct Lowerer {
            const RegAlloc      &ra;
            const TargetRegInfo &tri;
            bool     vm_abi = false;  ///< VM_ABI (salva RBX=ProcessVM*) vs host leaf
            uint32_t k = 0;          ///< numero de callee-saved asignados
            uint32_t total_saved = 0;///< callee-saved + (vm_abi ? 1 (rbx) : 0)
            int32_t  spill_bytes = 0;///< tamano del area de spills (alineado)
            /// Commit 8: offset (desde RBP) del inicio del area de allocas
            /// (justo debajo de los spill slots) + cursor de asignacion.
            uint32_t alloca_base = 0;
            uint32_t alloca_cursor = 0;
            MReg     scr0 = MReg::R10;
            MReg     scr1 = MReg::R11;

            Lowerer(const RegAlloc &r, const TargetRegInfo &t, AbiKind abi,
                    bool has_calls, uint32_t alloca_total)
                : ra(r), tri(t), vm_abi(abi == AbiKind::VM) {
                k = static_cast<uint32_t>(ra.callee_saved_used.size());
                total_saved = k + (vm_abi ? 1u : 0u);  // +1 por el push rbx
                /* Las allocas viven debajo de los spill slots. */
                alloca_base = 8u * total_saved + 8u * ra.num_spill_slots;
                spill_bytes = static_cast<int32_t>(
                    8u * ra.num_spill_slots + alloca_total);
#if defined(_WIN32)
                /* Win64: si hay CALLs, reservar 32 bytes de shadow/home space
                 * en el FONDO del frame (debajo de los spill slots) para que
                 * el callee no pise nuestros datos. */
                if (has_calls) spill_bytes += 32;
#else
                (void)has_calls;
#endif
                /* Alinear (8*total_saved + spill_bytes) a 16 para mantener el
                 * stack 16-aligned en CALLs internos. */
                if (((8u * total_saved) + static_cast<uint32_t>(spill_bytes)) % 16u != 0u)
                    spill_bytes += 8;
                const auto &sc = tri.scratch[static_cast<size_t>(RegClass::GP)];
                if (sc.size() >= 1) scr0 = static_cast<MReg>(sc[0]);
                if (sc.size() >= 2) scr1 = static_cast<MReg>(sc[1]);
            }

            /// Argumentos pendientes (idx, ubicacion) de la proxima CALL.
            std::vector<std::pair<uint8_t, MOperand>> pending_args;

            /// Phase D.7 commit 6: intervalos (para stackmaps en CALLs) +
            /// posicion lineal del CALL actual + stackmaps acumulados.
            const IntervalResult *ivs = nullptr;
            uint32_t cur_call_pos = 0;
            std::vector<Stackmap> stackmaps;

            /**
             * @brief Emite un PARALLEL MOVE: el conjunto de copias
             *        @c dst_reg <- src se realiza simultaneamente.  Emite en
             *        un orden seguro y rompe ciclos con @p scratch.
             */
            void emit_parallel_moves(
                std::vector<std::pair<MReg, MOperand>> moves,
                MReg scratch, std::vector<MInstr> &out) const {
                const size_t n = moves.size();
                std::vector<bool> done(n, false);
                size_t remaining = n;
                auto reads_dst = [&](size_t i) -> bool {
                    for (size_t j = 0; j < n; ++j) {
                        if (done[j] || j == i) continue;
                        if (moves[j].second.kind == MOperandKind::REG &&
                            moves[j].second.reg == reg_id(moves[i].first))
                            return true;
                    }
                    return false;
                };
                while (remaining > 0) {
                    bool progress = false;
                    for (size_t i = 0; i < n; ++i) {
                        if (done[i] || reads_dst(i)) continue;
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            reg(moves[i].first), moves[i].second));
                        done[i] = true; --remaining; progress = true;
                    }
                    if (progress) continue;
                    /* Ciclo: salvar un dst a scratch y romperlo. */
                    for (size_t i = 0; i < n; ++i) {
                        if (done[i]) continue;
                        const MReg d = moves[i].first;
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scratch), reg(d)));
                        for (size_t j = 0; j < n; ++j)
                            if (!done[j] && moves[j].second.kind == MOperandKind::REG &&
                                moves[j].second.reg == reg_id(d))
                                moves[j].second = reg(scratch);
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(d), moves[i].second));
                        done[i] = true; --remaining;
                        break;
                    }
                }
            }

            /** @brief Registro host que trae el @c ProcessVM* (primer arg). */
            static MReg proc_arg_reg() noexcept {
#if defined(_WIN32)
                return MReg::RCX;
#else
                return MReg::RDI;
#endif
            }

            /** @brief Offset desde RBP del spill slot @p slot. */
            int32_t slot_off(uint32_t slot) const noexcept {
                return -static_cast<int32_t>(8u * total_saved + 8u * (slot + 1u));
            }

            /** @brief Operando de memoria del spill slot @p slot: [rbp+off]. */
            MOperand slot_mem(uint32_t slot) const noexcept {
                return MOperand::make_mem(MReg::RBP, slot_off(slot));
            }

            /** @brief Resuelve un operando vreg a su ubicacion fisica. */
            MOperand resolve(const MOperand &o) const noexcept {
                if (!o.is_vreg()) return o;  // imm/label/mem/reg fisico: passthrough
                const uint32_t vid = o.vreg_id();
                if (ra.in_reg(vid))
                    return MOperand::make_reg(static_cast<MReg>(ra.reg_of(vid)), o.width);
                return slot_mem(ra.slot_of(vid));  // spilled
            }

            static MOperand reg(MReg r) { return MOperand::make_reg(r, 8); }
            static MInstr push(MReg r) {
                MInstr i; i.op = MOp::PUSH; i.src1 = reg(r); return i;
            }
            static MInstr pop(MReg r) {
                MInstr i; i.op = MOp::POP; i.dst = reg(r); return i;
            }

            void emit_prologue(std::vector<MInstr> &out) const {
                out.push_back(push(MReg::RBP));
                out.push_back(MInstr::make_unary(MOp::MOV, reg(MReg::RBP), reg(MReg::RSP)));
                if (vm_abi) {
                    /* Salvar RBX (callee-saved del host) y cargarlo con el
                     * ProcessVM* que llega en el primer arg. */
                    out.push_back(push(MReg::RBX));
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(MReg::RBX),
                                                     reg(proc_arg_reg())));
                }
                for (uint8_t r : ra.callee_saved_used)
                    out.push_back(push(static_cast<MReg>(r)));
                if (spill_bytes > 0)
                    out.push_back(MInstr::make_unary(MOp::SUB, reg(MReg::RSP),
                                                     MOperand::make_imm32(spill_bytes)));
            }

            void emit_epilogue(std::vector<MInstr> &out) const {
                /* lea rsp, [rbp - 8*total_saved] -> deshace el sub del frame y
                 * apunta rsp al ultimo registro salvado. */
                out.push_back(MInstr::make_unary(
                    MOp::LEA, reg(MReg::RSP),
                    MOperand::make_mem(MReg::RBP,
                        -static_cast<int32_t>(8u * total_saved))));
                /* pop callee en orden inverso. */
                for (size_t i = ra.callee_saved_used.size(); i-- > 0;)
                    out.push_back(pop(static_cast<MReg>(ra.callee_saved_used[i])));
                if (vm_abi) out.push_back(pop(MReg::RBX));
                out.push_back(pop(MReg::RBP));
            }

            /** @brief Reescribe una instr vreg a 0+ instrs fisicas. */
            void lower(const MInstr &in, std::vector<MInstr> &out) {
                const MOp op = in.op;

                if (op == MOp::ARG) {
                    /* Acumular: (indice, ubicacion fisica del vreg arg). */
                    pending_args.emplace_back(in.variant, resolve(in.src1));
                    return;
                }

                if (op == MOp::CALL_ABS) {
                    /* Parallel-move de los args a los registros de argumento
                     * del ABI host, luego mov scratch,addr + call scratch. */
                    const auto &areg = tri.arg_regs[static_cast<size_t>(RegClass::GP)];
                    std::vector<std::pair<MReg, MOperand>> moves;
                    for (const auto &pa : pending_args) {
                        if (pa.first < areg.size())
                            moves.emplace_back(static_cast<MReg>(areg[pa.first]), pa.second);
                        /* args >12 / en stack: no soportado v1 (raro). */
                    }
                    emit_parallel_moves(std::move(moves), scr1, out);
                    pending_args.clear();
                    /* Cargar la direccion (imm64 del pool) en scratch y llamar. */
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), in.src1));
                    MInstr call; call.op = MOp::CALL; call.src1 = reg(scr0);
                    /* Stackmap (commit 6): describir los GC roots vivos a
                     * traves de este call.  El linear_scan los forzo a slots,
                     * asi que estan en el stack -> el GC los lee via stackmap.
                     * Se asocia al CALL por @c flags (idx); el encoder rellena
                     * el pc_offset. */
                    if (ivs != nullptr) {
                        Stackmap sm;
                        const uint32_t NVI =
                            static_cast<uint32_t>(ivs->intervals.size());
                        for (uint32_t v = 0; v < NVI; ++v) {
                            const LiveInterval &lv = ivs->intervals[v];
                            if (!lv.is_gc() || !lv.covers(cur_call_pos)) continue;
                            if (!ra.spilled(v)) continue;  // invariante: GC+cross-call -> slot
                            StackmapSlot s;
                            s.rbp_offset = static_cast<int16_t>(slot_off(ra.slot_of(v)));
                            s.gc_kind = static_cast<StackmapGcKind>(
                                static_cast<uint8_t>(lv.gc_kind - 1u));
                            sm.slots.push_back(s);
                        }
                        call.flags = static_cast<uint16_t>(stackmaps.size());
                        stackmaps.push_back(std::move(sm));
                    }
                    out.push_back(call);
                    return;
                }

                if (op == MOp::RET) {
                    emit_epilogue(out);
                    out.push_back(MInstr::make_ret());
                    return;
                }

                if (op == MOp::DIVMOD_V) {
                    /* dst = src1 / src2 (variant 0) o src1 % src2 (variant 1),
                     * signed.  Secuencia x86 con RAX:RDX fijos.  El divisor va a
                     * R11 (scr1, reservado -> nunca es un operando, sin aliasing)
                     * ANTES de tocar RAX/RDX, asi cualquier asignacion fisica de
                     * los operandos (incluida RAX/RDX) es segura:
                     *   mov  r11, divisor      ; divisor leido antes de clobbers
                     *   mov  rax, dividendo    ; (no-op si ya en RAX)
                     *   cqo                    ; sign-extend RAX -> RDX:RAX
                     *   idiv r11               ; RAX=cociente, RDX=resto
                     *   mov  dst, rax|rdx
                     * DIVMOD_V es call-position -> ningun vreg vivo a traves esta
                     * en RAX/RDX (van a callee-saved), asi el clobber es seguro. */
                    const MOperand a = resolve(in.src1);   // dividendo
                    const MOperand b = resolve(in.src2);   // divisor
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), b));
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(MReg::RAX), a));
                    { MInstr c; c.op = MOp::CQO; out.push_back(c); }
                    out.push_back(MInstr::make_unary(MOp::IDIV, MOperand{},
                                                     reg(scr1)));
                    const MReg res = (in.variant == 1u) ? MReg::RDX : MReg::RAX;
                    out.push_back(MInstr::make_unary(MOp::MOV,
                                                     resolve(in.dst), reg(res)));
                    return;
                }

                if (op == MOp::LOAD) {
                    /* dst = [addr].  addr y dst pueden estar spilled. */
                    const uint8_t width = static_cast<uint8_t>(in.flags >> 1);
                    const bool    sgn   = (in.flags & 1u) != 0u;
                    MOperand a = resolve(in.src1);
                    MReg addr_reg;
                    if (a.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), a));
                        addr_reg = scr1;
                    } else {
                        addr_reg = static_cast<MReg>(a.reg);
                    }
                    /* NO tocar mem.width: empaqueta scale|index del MEM.  El
                     * ancho de un MOV [mem] lo da el reg; el de MOVSX/MOVZX va
                     * en mem.flags (mem_size override). */
                    MOperand mem = MOperand::make_mem(addr_reg, 0);
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
                    if (width == 8) {
                        out.push_back(MInstr::make_unary(MOp::MOV, pdst, mem));
                    } else if (sgn) {
                        mem.flags = width;  // ancho del src para MOVSX
                        out.push_back(MInstr::make_unary(MOp::MOVSX, pdst, mem));
                    } else {  // u8/u16 (u32 lo filtro el selector)
                        mem.flags = width;
                        out.push_back(MInstr::make_unary(MOp::MOVZX, pdst, mem));
                    }
                    if (dst_spilled)
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                    return;
                }

                if (op == MOp::STORE) {
                    /* [addr] = val.  addr -> scr1 si spilled; val -> scr0 si spilled. */
                    const uint8_t width = static_cast<uint8_t>(in.flags);
                    MOperand a = resolve(in.src1);
                    MReg addr_reg;
                    if (a.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), a));
                        addr_reg = scr1;
                    } else {
                        addr_reg = static_cast<MReg>(a.reg);
                    }
                    MOperand v = resolve(in.src2);
                    if (v.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), v));
                        v = reg(scr0);
                    }
                    v.width = width;  // ancho del store lo da el reg src
                    /* NO tocar mem.width (index packing). */
                    MOperand mem = MOperand::make_mem(addr_reg, 0);
                    out.push_back(MInstr::make_unary(MOp::MOV, mem, v));
                    return;
                }

                if (op == MOp::ALLOCA) {
                    /* dst = host_ptr a `size` bytes reservados en el frame
                     * (debajo de los spills).  LEA dst, [rbp - off]. */
                    const uint32_t size =
                        static_cast<uint32_t>(in.src1.value);
                    const uint32_t aligned = (size + 7u) & ~7u;
                    const int32_t off = static_cast<int32_t>(
                        alloca_base + alloca_cursor + aligned);
                    alloca_cursor += aligned;
                    const MOperand mem = MOperand::make_mem(MReg::RBP, -off);
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
                    out.push_back(MInstr::make_unary(MOp::LEA, pdst, mem));
                    if (dst_spilled)
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                    return;
                }

                if (op == MOp::MOV) {
                    const MOperand rs = resolve(in.src1);
                    if (in.dst.is_vreg() && ra.spilled(in.dst.vreg_id())) {
                        const uint32_t slot = ra.slot_of(in.dst.vreg_id());
                        if (rs.kind == MOperandKind::REG) {
                            out.push_back(MInstr::make_unary(MOp::MOV, slot_mem(slot), rs));
                        } else {
                            /* mem<-mem o mem<-imm: pasar por scratch. */
                            out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), rs));
                            out.push_back(MInstr::make_unary(MOp::MOV, slot_mem(slot), reg(scr0)));
                        }
                    } else {
                        /* dst no-spilled (reg) o dst MEM fisico (p.ej. el
                         * return VM a [rbx+off]).  Si AMBOS son MEM (dst MEM
                         * fisico + src spilled), pasar por scratch (no hay
                         * mov mem,mem en x86). */
                        const MOperand d = resolve(in.dst);
                        if (d.kind == MOperandKind::MEM && rs.kind == MOperandKind::MEM) {
                            out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), rs));
                            out.push_back(MInstr::make_unary(MOp::MOV, d, reg(scr0)));
                        } else {
                            out.push_back(MInstr::make_unary(MOp::MOV, d, rs));
                        }
                    }
                    return;
                }

                if (op == MOp::MOVZX || op == MOp::MOVSX) {
                    /* Extension: dst64 <- src<width>.  El encoder exige
                     * dst=REG; si el src esta spilled, cargarlo a scratch1
                     * preservando su width; si el dst esta spilled, extender
                     * a scratch0 y almacenar. */
                    MOperand rs = resolve(in.src1);
                    if (rs.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), rs));
                        rs = MOperand::make_reg(scr1, in.src1.width);  // width del src
                    }
                    const bool dst_spilled = in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
                    MInstr ext; ext.op = op; ext.dst = pdst; ext.src1 = rs;
                    out.push_back(ext);
                    if (dst_spilled)
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                    return;
                }

                if (op == MOp::SHL || op == MOp::SHR || op == MOp::SAR
                 || op == MOp::ROL || op == MOp::ROR) {
                    /* dst = src1 (sh/rot) src2(imm).  MOV dst, src1; SHL dst, imm.
                     * Solo cantidad INMEDIATA (el selector emite ROL/ROR aqui solo
                     * con count constante; variable -> fallback, requiere CL). */
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
                    const MOperand rs1 = resolve(in.src1);
                    if (!(pdst.kind == MOperandKind::REG &&
                          rs1.kind == MOperandKind::REG && pdst.reg == rs1.reg))
                        out.push_back(MInstr::make_unary(MOp::MOV, pdst, rs1));
                    MInstr sh; sh.op = op; sh.dst = pdst; sh.src1 = in.src2;  // imm
                    out.push_back(sh);
                    if (dst_spilled)
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                    return;
                }

                if (op == MOp::LZCNT || op == MOp::TZCNT || op == MOp::POPCNT) {
                    /* dst = op(src).  dst debe ser REG (si spilled -> scratch
                     * + store).  src puede ser MEM (op r64, r/m64). */
                    const MOperand rs = resolve(in.src1);
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
                    out.push_back(MInstr::make_unary(op, pdst, rs));
                    if (dst_spilled)
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                    return;
                }

                if (op == MOp::BSWAP) {
                    /* bswap r64: in-place, dst debe ser REG.  Si dst spilled,
                     * cargar a scratch, bswap, store.  src1 == dst (mismo vreg). */
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    if (dst_spilled) {
                        const MOperand sl = slot_mem(ra.slot_of(in.dst.vreg_id()));
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), sl));
                        out.push_back(MInstr::make_unary(MOp::BSWAP,
                            reg(scr0), reg(scr0)));
                        out.push_back(MInstr::make_unary(MOp::MOV, sl, reg(scr0)));
                    } else {
                        const MOperand pdst = resolve(in.dst);
                        out.push_back(MInstr::make_unary(MOp::BSWAP, pdst, pdst));
                    }
                    return;
                }

                if (op == MOp::CMOVCC) {
                    /* dst = (cc) ? src : dst  (read-modify).  dst debe ser REG.
                     * Preserva variant (codigo de condicion).  src spilled -> scr1;
                     * dst spilled -> carga a scr0, cmov, store. */
                    MOperand rsrc = resolve(in.src1);
                    if (rsrc.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), rsrc));
                        rsrc = reg(scr1);
                    }
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    MInstr c; c.op = MOp::CMOVCC; c.variant = in.variant;
                    if (dst_spilled) {
                        const MOperand sl = slot_mem(ra.slot_of(in.dst.vreg_id()));
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), sl));
                        c.dst = reg(scr0); c.src1 = rsrc;
                        out.push_back(c);
                        out.push_back(MInstr::make_unary(MOp::MOV, sl, reg(scr0)));
                    } else {
                        c.dst = resolve(in.dst); c.src1 = rsrc;
                        out.push_back(c);
                    }
                    return;
                }

                if (is_bin_alu(op)) {
                    const bool dst_spilled = in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
                    const MOperand rs1  = resolve(in.src1);
                    const MOperand rs2  = resolve(in.src2);

                    const bool anti = (rs2.kind == MOperandKind::REG &&
                                       pdst.kind == MOperandKind::REG &&
                                       rs2.reg == pdst.reg);
                    if (anti) {
                        if (is_commutative(op)) {
                            /* pdst ya contiene src2 -> OP pdst, src1 (conmutativo). */
                            out.push_back(MInstr::make_unary(op, pdst, rs1));
                        } else {
                            /* SUB y pdst==src2reg: usar scratch1 para src2. */
                            out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), rs2));
                            out.push_back(MInstr::make_unary(MOp::MOV, pdst, rs1));
                            out.push_back(MInstr::make_unary(op, pdst, reg(scr1)));
                        }
                    } else {
                        out.push_back(MInstr::make_unary(MOp::MOV, pdst, rs1)); // pdst = src1
                        out.push_back(MInstr::make_unary(op, pdst, rs2));       // pdst OP= src2
                    }
                    if (dst_spilled) {
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                    }
                    return;
                }

                if (op == MOp::CMP || op == MOp::TEST) {
                    MOperand a = resolve(in.src1);
                    MOperand b = resolve(in.src2);
                    if (a.kind == MOperandKind::MEM && b.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), a));
                        a = reg(scr0);
                    }
                    out.push_back(MInstr::make_unary(op, a, b));  // dst=a, src1=b
                    return;
                }

                /* Resto (JMP/JCC/LABEL_DEF/NOP/...): sustituir vregs en sitio. */
                MInstr m = in;
                m.dst  = resolve(in.dst);
                m.src1 = resolve(in.src1);
                m.src2 = resolve(in.src2);
                out.push_back(m);
            }
        };

    } // namespace

    MFunction rewrite_to_physical(const MFunction &vf,
                                  const RegAlloc &ra,
                                  const TargetRegInfo &tri,
                                  AbiKind abi,
                                  const IntervalResult *ivs) {
        /* Detectar si la funcion tiene CALLs (para reservar shadow space). */
        bool has_calls = false;
        uint32_t alloca_total = 0;  // commit 8: bytes de allocas en el frame
        for (const auto &b : vf.blocks) {
            for (const auto &in : b.instrs) {
                if (in.op == MOp::CALL || in.op == MOp::CALL_ABS) has_calls = true;
                if (in.op == MOp::ALLOCA) {
                    const uint32_t sz = static_cast<uint32_t>(in.src1.value);
                    alloca_total += (sz + 7u) & ~7u;
                }
            }
        }
        Lowerer lw(ra, tri, abi, has_calls, alloca_total);
        lw.ivs = ivs;  // commit 6: para construir stackmaps en CALLs
        MFunction pf;
        pf.name          = vf.name;
        pf.next_label_id  = vf.next_label_id;
        pf.label_offsets  = vf.label_offsets;
        pf.imm64_pool     = vf.imm64_pool;
        pf.blocks.resize(vf.blocks.size());

        /* gi = indice lineal global de la instr vreg (MISMO orden que
         * build_intervals).  cur_call_pos = 2*gi se pasa a lower() para que
         * el CALL_ABS sepa que GC roots estan vivos en ese punto. */
        uint32_t gi = 0;
        for (size_t b = 0; b < vf.blocks.size(); ++b) {
            pf.blocks[b].label_id = vf.blocks[b].label_id;
            pf.blocks[b].succ_a   = vf.blocks[b].succ_a;
            pf.blocks[b].succ_b   = vf.blocks[b].succ_b;
            std::vector<MInstr> outv;
            outv.reserve(vf.blocks[b].instrs.size() * 2 + 8);
            if (b == 0) lw.emit_prologue(outv);
            for (const MInstr &in : vf.blocks[b].instrs) {
                lw.cur_call_pos = 2u * gi;
                lw.lower(in, outv);
                ++gi;
            }
            pf.blocks[b].instrs = std::move(outv);
        }
        pf.stackmaps = std::move(lw.stackmaps);  // commit 6
        return pf;
    }

} // namespace jit
