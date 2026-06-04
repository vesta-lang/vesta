/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/vreg_select.cpp
 * @brief Implementacion del selector vreg (Phase D.7, commits 4b/4c).
 *        Ver vreg_select.h y doc/REGALLOC.md.
 *
 * Commit 4b: CONST + ALU + RET (1 bloque).
 * Commit 4c: multi-bloque con control de flujo (CMP_* / BR / BR_COND) y PHI
 * (resuelto via copias en predecesores).  La compare-and-branch se FUSIONA a
 * CMP + Jcc cuando el cond de un BR_COND lo produce un CMP_* adyacente;
 * si no, se materializa el bool (CMP + MOV 0 + SETcc) y el BR_COND hace
 * TEST + Jcc.
 *
 * Limitacion 4c: las aristas criticas (un BR_COND cuyo target tiene PHIs)
 * NO se soportan todavia -> @c vreg_select devuelve false (fallback).  El
 * caso comun de loops/if-else generados por el frontend NO las produce.
 */

#include "jit/vreg_select.h"

#include "ir/ssa_ir.h"
#include "vesta_rt/abi.h"

#include <cstdio>
#include <cstdlib>

namespace jit {

    /** @brief Diagnostico opt-in (VESTA_JIT_VREGS_DEBUG=1) de por que una
     *  funcion no es seleccionable por el path vreg. */
    static void vreg_dbg(const char *fn, const char *op) {
        static const bool on = []{
            const char *v = std::getenv("VESTA_JIT_VREGS_DEBUG");
            return v && v[0] != '\0' && v[0] != '0';
        }();
        if (on) std::fprintf(stderr, "[vreg-sel] '%s' no soportada: op %s\n", fn, op);
    }

    namespace {

        /** @brief Operando VREG (clase GP) para un IrValueId. */
        inline MOperand vr(ir::IrValueId v) {
            return MOperand::make_vreg(static_cast<uint32_t>(v), RegClass::GP, 8);
        }

        /** @brief Tamano en bytes de un IrType entero/puntero. */
        inline int ir_type_bytes(ir::IrType t) {
            switch (t) {
                case ir::IrType::I8: case ir::IrType::U8: case ir::IrType::BOOL: return 1;
                case ir::IrType::I16: case ir::IrType::U16: return 2;
                case ir::IrType::I32: case ir::IrType::U32: case ir::IrType::F32: return 4;
                default: return 8;  // I64/U64/F64/PTR/HANDLE
            }
        }
        /** @brief True si el tipo entero tiene signo. */
        inline bool ir_type_signed(ir::IrType t) {
            return t == ir::IrType::I8 || t == ir::IrType::I16 ||
                   t == ir::IrType::I32 || t == ir::IrType::I64;
        }
        inline bool ir_type_is_float(ir::IrType t) {
            return t == ir::IrType::F32 || t == ir::IrType::F64;
        }

        /** @brief Operando memoria [rbx + proc->registers.regs[j]] (VM_ABI).
         *  RBX = ProcessVM* durante la funcion.  j=0 retorno, j>=1 argumentos. */
        inline MOperand vm_reg_mem(int j) {
            return MOperand::make_mem(MReg::RBX,
                VESTA_PROC_REGISTERS_OFFSET + j * VESTA_REGISTER_SIZE);
        }

        /** @brief Mapea un IrOp de ALU binaria al MOp x86, o false si no aplica. */
        inline bool bin_mop(ir::IrOp op, MOp &out) {
            switch (op) {
                case ir::IrOp::ADD: out = MOp::ADD;  return true;
                case ir::IrOp::SUB: out = MOp::SUB;  return true;
                case ir::IrOp::MUL: out = MOp::IMUL; return true;
                case ir::IrOp::AND: out = MOp::AND;  return true;
                case ir::IrOp::OR:  out = MOp::OR;   return true;
                case ir::IrOp::XOR: out = MOp::XOR;  return true;
                default:            return false;
            }
        }

        /** @brief Condicion x86 para un CMP_* del IR, o false si no es CMP. */
        inline bool cmp_cond(ir::IrOp op, MCond &cc) {
            switch (op) {
                case ir::IrOp::CMP_EQ:  cc = MCond::E;  return true;
                case ir::IrOp::CMP_NE:  cc = MCond::NE; return true;
                case ir::IrOp::CMP_LT:  cc = MCond::L;  return true;  // signed
                case ir::IrOp::CMP_GT:  cc = MCond::G;  return true;
                case ir::IrOp::CMP_LE:  cc = MCond::LE; return true;
                case ir::IrOp::CMP_GE:  cc = MCond::GE; return true;
                case ir::IrOp::CMP_ULT: cc = MCond::B;  return true;  // unsigned
                case ir::IrOp::CMP_UGT: cc = MCond::A;  return true;
                case ir::IrOp::CMP_ULE: cc = MCond::BE; return true;
                case ir::IrOp::CMP_UGE: cc = MCond::AE; return true;
                default:                return false;
            }
        }

        /** @brief MInstr CMP a, b (forma 3-op: src1=a, src2=b; el rewrite la baja). */
        inline MInstr mk_cmp(ir::IrValueId a, ir::IrValueId b) {
            MInstr i; i.op = MOp::CMP; i.src1 = vr(a); i.src2 = vr(b); return i;
        }
        /** @brief MInstr TEST a, b. */
        inline MInstr mk_test(ir::IrValueId a, ir::IrValueId b) {
            MInstr i; i.op = MOp::TEST; i.src1 = vr(a); i.src2 = vr(b); return i;
        }
        /** @brief MInstr SETcc dst (variant=cc). */
        inline MInstr mk_setcc(ir::IrValueId dst, MCond cc) {
            MInstr i; i.op = MOp::SETCC; i.dst = vr(dst);
            i.variant = static_cast<uint8_t>(cc); return i;
        }

        /** @brief True si el bloque @p b tiene alguna instr PHI. */
        inline bool block_has_phi(const ir::IrBlock &b) {
            for (const auto &in : b.instrs)
                if (in.op == ir::IrOp::PHI) return true;
            return false;
        }

    } // namespace

    bool vreg_select(const ir::IrFunction &fn, MFunction &out, AbiKind abi,
                     const CallResolver &resolve_call, uint64_t callvirt_addr,
                     uint64_t gc_deref_addr, uint64_t gc_handle_addr,
                     uint64_t raw_alloc_addr) {
        out = MFunction{};
        out.name = fn.name;
        out.vreg_count = static_cast<uint32_t>(fn.values.size());
        out.vreg_class.assign(fn.values.size(), RegClass::GP);
        const bool vm = (abi == AbiKind::VM);

        /* Phase D.7 commit 5f: marcar los vregs GC.  El pipeline hace el
         * check FINO (sin stackmaps todavia): rechaza la funcion solo si un
         * valor GC esta VIVO a traves de un call (su intervalo cubre un
         * call_position) -- ese seria invisible al GC en un registro.  Los
         * receptores/args de un call van a proc->registers (que el GC SI
         * escanea) y mueren antes del call, asi que no disparan el rechazo. */
        out.vreg_is_gc.assign(fn.values.size(), 0);
        for (size_t i = 0; i < fn.values.size(); ++i) {
            if (!fn.values[i].is_gc_object) continue;
            /* Codifica StackmapGcKind+1: HOSTPTR si es host_ptr a objeto GC,
             * HANDLE en otro caso.  (STRING se trata como HOSTPTR: el scan
             * usa el mismo host_ptr al payload.) */
            const StackmapGcKind k = fn.values[i].is_host_ptr
                ? StackmapGcKind::HOSTPTR : StackmapGcKind::HANDLE;
            out.vreg_is_gc[i] = static_cast<uint8_t>(static_cast<uint8_t>(k) + 1u);
        }

        const size_t NB = fn.blocks.size();
        if (NB == 0) return false;

        /* Un label por bloque (MBlock index == IR block id). */
        std::vector<MLabelId> blbl(NB);
        for (size_t b = 0; b < NB; ++b) blbl[b] = out.new_label();

        /* Valores const conocidos (para shifts por cantidad inmediata).  El
         * IR define la cantidad de un shift con un CONST previo; lo capturamos
         * para emitir SHL/SHR/SAR dst, imm en vez de via CL. */
        std::vector<uint8_t>  v_is_const(fn.values.size(), 0);
        std::vector<int64_t>  v_const(fn.values.size(), 0);

        for (size_t b = 0; b < NB; ++b) {
            const ir::IrBlock &ib = fn.blocks[b];
            MBlock mb;
            mb.label_id = blbl[b];
            auto &O = mb.instrs;

            /* VM_ABI: al entrar, cargar cada parametro desde
             * proc->registers.regs[i+1] (regs[0] reservado al retorno). */
            if (vm && b == 0) {
                for (size_t i = 0; i < fn.params.size(); ++i)
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(fn.params[i]),
                        vm_reg_mem(static_cast<int>(i) + 1)));
            }

            /* Emite las copias de PHI para una arista (b -> target): por cada
             * PHI del target, MOV phi_dst <- (arg cuyo pred == b). */
            auto emit_phi_copies = [&](ir::IrBlockId target) {
                const ir::IrBlock &tb = fn.blocks[target];
                for (const ir::IrInstr &p : tb.instrs) {
                    if (p.op != ir::IrOp::PHI) continue;
                    for (const ir::IrPhiArg &a : p.phi_args) {
                        if (a.block == static_cast<ir::IrBlockId>(b)) {
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(p.dst), vr(a.value)));
                            break;
                        }
                    }
                }
            };

            /* CMP diferido para fusionar compare-and-branch. */
            bool      has_pend = false;
            MCond     pend_cc  = MCond::E;
            ir::IrValueId pend_dst = ir::IR_NO_VALUE, pend_a = 0, pend_b = 0;
            auto flush_pending = [&]() {
                if (!has_pend) return;
                O.push_back(mk_cmp(pend_a, pend_b));
                O.push_back(MInstr::make_unary(MOp::MOV, vr(pend_dst),
                                               MOperand::make_imm32(0)));
                O.push_back(mk_setcc(pend_dst, pend_cc));
                has_pend = false;
            };

            for (const ir::IrInstr &in : ib.instrs) {
                MOp  mop;
                MCond cc;
                if (in.op == ir::IrOp::PHI) continue;  // resuelto via copias

                if (cmp_cond(in.op, cc)) {
                    flush_pending();
                    if (in.operands.size() != 2) return false;
                    /* Diferir: quiza se fusione con el BR_COND siguiente. */
                    has_pend = true; pend_cc = cc; pend_dst = in.dst;
                    pend_a = in.operands[0]; pend_b = in.operands[1];
                    continue;
                }

                switch (in.op) {
                    case ir::IrOp::NOP: break;

                    case ir::IrOp::CONST: {
                        flush_pending();
                        if (in.dst < v_is_const.size()) {  // recordar para shifts
                            v_is_const[in.dst] = 1;
                            v_const[in.dst] = static_cast<int64_t>(in.imm);
                        }
                        const int64_t s = static_cast<int64_t>(in.imm);
                        if (s >= INT32_MIN && s <= INT32_MAX) {
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                MOperand::make_imm32(static_cast<int32_t>(s))));
                        } else {
                            const uint32_t idx = out.intern_imm64(in.imm);
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                MOperand::make_imm64_idx(idx)));
                        }
                        break;
                    }

                    case ir::IrOp::ADD: case ir::IrOp::SUB: case ir::IrOp::MUL:
                    case ir::IrOp::AND: case ir::IrOp::OR:  case ir::IrOp::XOR: {
                        flush_pending();
                        if (in.operands.size() != 2) return false;
                        (void)bin_mop(in.op, mop);
                        O.push_back(MInstr::make_binary(mop, vr(in.dst),
                            vr(in.operands[0]), vr(in.operands[1])));
                        break;
                    }
                    case ir::IrOp::NEG: {
                        flush_pending();
                        if (in.operands.size() != 1) return false;
                        O.push_back(MInstr::make_unary(MOp::NEG, vr(in.dst),
                            vr(in.operands[0])));
                        break;
                    }
                    case ir::IrOp::NOT: {
                        flush_pending();
                        if (in.operands.size() != 1) return false;
                        O.push_back(MInstr::make_unary(MOp::NOT, vr(in.dst),
                            vr(in.operands[0])));
                        break;
                    }

                    /* SHL/SHR/SAR con cantidad INMEDIATA (el caso comun: el IR
                     * define la cuenta con un CONST).  dst = src << imm.  Cuenta
                     * variable (en registro) -> fallback (necesita CL). */
                    case ir::IrOp::SHL: case ir::IrOp::SHR: case ir::IrOp::SAR: {
                        flush_pending();
                        if (in.operands.size() != 2) return false;
                        const ir::IrValueId amt_v = in.operands[1];
                        if (amt_v >= v_is_const.size() || !v_is_const[amt_v]) {
                            vreg_dbg(fn.name.c_str(), "shift-var"); return false;
                        }
                        const int32_t amt = static_cast<int32_t>(v_const[amt_v] & 63);
                        const MOp mop = (in.op == ir::IrOp::SHL) ? MOp::SHL
                                      : (in.op == ir::IrOp::SHR) ? MOp::SHR : MOp::SAR;
                        O.push_back(MInstr::make_binary(mop, vr(in.dst),
                            vr(in.operands[0]), MOperand::make_imm32(amt)));
                        break;
                    }

                    /* Conversiones enteras: BITCAST (mismo ancho -> MOV),
                     * SEXT/ZEXT/CAST (extension via MOVSX/MOVZX).  Las
                     * truncaciones y los floats caen a fallback por ahora. */
                    case ir::IrOp::BITCAST: {
                        flush_pending();
                        if (in.operands.size() != 1) return false;
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                            vr(in.operands[0])));
                        break;
                    }
                    case ir::IrOp::ZEXT: case ir::IrOp::SEXT: case ir::IrOp::CAST:
                    case ir::IrOp::TRUNC: {
                        flush_pending();
                        if (in.operands.size() != 1) return false;
                        const ir::IrType st = fn.values[in.operands[0]].type;
                        const ir::IrType dt = in.type;
                        if (ir_type_is_float(st) || ir_type_is_float(dt)) {
                            vreg_dbg(fn.name.c_str(), ir::ir_op_name(in.op));
                            return false;
                        }
                        const int sb = ir_type_bytes(st), db = ir_type_bytes(dt);
                        /* MOV de ancho w (w<8 zero-extiende los bits altos). */
                        auto mov_w = [&](int w) {
                            MOperand d = vr(in.dst);            d.width = static_cast<uint8_t>(w);
                            MOperand s = vr(in.operands[0]);    s.width = static_cast<uint8_t>(w);
                            O.push_back(MInstr::make_unary(MOp::MOV, d, s));
                        };
                        /* MOVSX/MOVZX dst64 <- src<srcw>. */
                        auto ext = [&](MOp mop, int srcw) {
                            const MOperand s = MOperand::make_vreg(
                                static_cast<uint32_t>(in.operands[0]),
                                RegClass::GP, static_cast<uint8_t>(srcw));
                            O.push_back(MInstr::make_unary(mop, vr(in.dst), s));
                        };
                        if (db == sb) {
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                vr(in.operands[0])));  // copia de bits
                        } else if (db > sb) {
                            /* EXTENSION (zext/sext). */
                            const bool sign = (in.op == ir::IrOp::SEXT) ||
                                (in.op == ir::IrOp::CAST && ir_type_signed(st));
                            if (sb == 1 || sb == 2) ext(sign ? MOp::MOVSX : MOp::MOVZX, sb);
                            else if (sign)          ext(MOp::MOVSX, 4);  // i32->i64 (MOVSXD)
                            else                    mov_w(4);            // u32->u64 zero-ext
                        } else {
                            /* TRUNCACION (db < sb): el signo lo da el DESTINO. */
                            const bool sign = ir_type_signed(dt);
                            if (db == 4) {
                                if (sign) ext(MOp::MOVSX, 4);  // i32: 32 bajos sign-ext
                                else      mov_w(4);            // u32: 32 bajos zero-ext
                            } else {  // db == 1 || db == 2
                                ext(sign ? MOp::MOVSX : MOp::MOVZX, db);
                            }
                        }
                        break;
                    }

                    case ir::IrOp::BR: {
                        flush_pending();
                        const ir::IrBlockId t = in.target_block;
                        emit_phi_copies(t);                       // pred 1-succ: seguro
                        O.push_back(MInstr::make_jmp(blbl[t]));
                        mb.succ_a = static_cast<MBlockId>(t);
                        break;
                    }

                    case ir::IrOp::BR_COND: {
                        if (in.operands.size() != 1) return false;
                        const ir::IrBlockId tt = in.target_block;   // true
                        const ir::IrBlockId tf = in.false_block;    // false
                        /* Arista critica (target con PHIs) no soportada en 4c. */
                        if (block_has_phi(fn.blocks[tt]) ||
                            block_has_phi(fn.blocks[tf])) return false;
                        const ir::IrValueId cond = in.operands[0];

                        if (has_pend && pend_dst == cond) {
                            /* FUSION: CMP a,b + Jcc(cc) true + JMP false. */
                            O.push_back(mk_cmp(pend_a, pend_b));
                            O.push_back(MInstr::make_jcc(pend_cc, blbl[tt]));
                            O.push_back(MInstr::make_jmp(blbl[tf]));
                            has_pend = false;
                        } else {
                            flush_pending();
                            O.push_back(mk_test(cond, cond));
                            O.push_back(MInstr::make_jcc(MCond::NE, blbl[tt]));
                            O.push_back(MInstr::make_jmp(blbl[tf]));
                        }
                        mb.succ_a = static_cast<MBlockId>(tt);
                        mb.succ_b = static_cast<MBlockId>(tf);
                        break;
                    }

                    case ir::IrOp::RET: {
                        flush_pending();
                        if (!in.operands.empty()) {
                            /* VM_ABI: escribir el resultado en
                             * proc->registers.regs[0] ([rbx+off]); host leaf:
                             * dejarlo en RAX. */
                            const MOperand dst = vm
                                ? vm_reg_mem(0)
                                : MOperand::make_reg(MReg::RAX, 8);
                            O.push_back(MInstr::make_unary(MOp::MOV, dst,
                                vr(in.operands[0])));
                        }
                        O.push_back(MInstr::make_ret());
                        break;
                    }

                    /* ALLOCA host (auto-promote, no escapa): reserva en el frame
                     * JIT -> dst = host_ptr.  Las allocas que van al VM stack
                     * (host_alloca=false) caen a fallback. */
                    case ir::IrOp::ALLOCA: {
                        flush_pending();
                        if (!in.host_alloca) {
                            vreg_dbg(fn.name.c_str(), "alloca-vm"); return false;
                        }
                        const uint64_t size = in.imm;
                        if (size == 0 || size > 65536) {  // sanity (frame chico)
                            vreg_dbg(fn.name.c_str(), "alloca-size"); return false;
                        }
                        O.push_back(MInstr::make_alloca(vr(in.dst),
                            static_cast<uint32_t>(size)));
                        break;
                    }

                    /* LOAD/STORE sobre memoria HOST (host_ptr): field access de
                     * objetos GC, malloc, etc.  La direccion ya viene calculada
                     * en un vreg (el IR emite add.ptr this+offset).  vm_mem
                     * (is_host_ptr=false) cae a fallback (necesita traduccion). */
                    case ir::IrOp::LOAD: {
                        flush_pending();
                        if (in.operands.size() != 1) return false;
                        if (ir_type_is_float(in.type)) {
                            vreg_dbg(fn.name.c_str(), "load-float"); return false;
                        }
                        if (!fn.values[in.operands[0]].is_host_ptr) {
                            vreg_dbg(fn.name.c_str(), "load-vm"); return false;
                        }
                        const int w = ir_type_bytes(in.type);
                        /* u32 (4 bytes unsigned) necesita mov de 32 bits zero-ext;
                         * no soportado aun -> fallback. */
                        if (w == 4 && !ir_type_signed(in.type)) {
                            vreg_dbg(fn.name.c_str(), "load-u32"); return false;
                        }
                        O.push_back(MInstr::make_load(vr(in.dst),
                            vr(in.operands[0]), static_cast<uint8_t>(w),
                            ir_type_signed(in.type)));
                        break;
                    }
                    case ir::IrOp::STORE: {
                        flush_pending();
                        if (in.operands.size() != 2) return false;  // [0]=val [1]=ptr
                        if (ir_type_is_float(in.type)) {
                            vreg_dbg(fn.name.c_str(), "store-float"); return false;
                        }
                        if (!fn.values[in.operands[1]].is_host_ptr) {
                            vreg_dbg(fn.name.c_str(), "store-vm"); return false;
                        }
                        const int w = ir_type_bytes(in.type);
                        O.push_back(MInstr::make_store(vr(in.operands[1]),
                            vr(in.operands[0]), static_cast<uint8_t>(w)));
                        break;
                    }

                    /* GC_DEREF_HOST: dst = vrt_gc_deref(proc, handle).
                     * GC_HANDLE_FOR_PTR: dst = vrt_gc_handle_for_ptr(proc, ptr).
                     * Ambos son lookups (NO disparan GC).  Convencion host:
                     * proc=arg0, valor=arg1, resultado en RAX. */
                    case ir::IrOp::GC_DEREF_HOST:
                    case ir::IrOp::GC_HANDLE_FOR_PTR:
                    case ir::IrOp::RAW_ALLOC: {
                        flush_pending();
                        const uint64_t addr =
                            (in.op == ir::IrOp::GC_DEREF_HOST)     ? gc_deref_addr  :
                            (in.op == ir::IrOp::GC_HANDLE_FOR_PTR) ? gc_handle_addr :
                                                                     raw_alloc_addr;
                        if (!vm || addr == 0) {
                            vreg_dbg(fn.name.c_str(), "gc_runtime"); return false;
                        }
                        if (in.operands.size() != 1) return false;
#if defined(_WIN32)
                        const MReg ga0 = MReg::RCX, ga1 = MReg::RDX;
#else
                        const MReg ga0 = MReg::RDI, ga1 = MReg::RSI;
#endif
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(ga1, 8), vr(in.operands[0])));  // valor primero
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(ga0, 8), MOperand::make_reg(MReg::RBX, 8)));
                        O.push_back(MInstr::make_call_abs(out.intern_imm64(addr)));
                        if (in.dst != ir::IR_NO_VALUE)
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                MOperand::make_reg(MReg::RAX, 8)));
                        break;
                    }

                    /* CALL intra-modulo a otra funcion Vex (VM_ABI): los args
                     * van a proc->registers.regs[1..N] (NO a arg_regs host),
                     * proc en RCX/RDI, resultado en regs[0]. */
                    case ir::IrOp::CALL: {
                        flush_pending();
                        if (!vm || !resolve_call) {
                            vreg_dbg(fn.name.c_str(), "call(no-vm/no-resolver)");
                            return false;
                        }
                        const uint64_t addr = resolve_call(in.func_name);
                        if (addr == 0) {
                            vreg_dbg(fn.name.c_str(), "call-unresolved");
                            return false;
                        }
                        /* 1. Stores de args a proc->registers.regs[i+1]. */
                        for (size_t i = 0; i < in.operands.size(); ++i)
                            O.push_back(MInstr::make_unary(MOp::MOV,
                                vm_reg_mem(static_cast<int>(i) + 1),
                                vr(in.operands[i])));
                        /* 2. proc (=RBX) al primer arg host del callee. */
#if defined(_WIN32)
                        const MReg proc_reg = MReg::RCX;
#else
                        const MReg proc_reg = MReg::RDI;
#endif
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(proc_reg, 8),
                            MOperand::make_reg(MReg::RBX, 8)));
                        /* 3. CALL a la direccion resuelta. */
                        O.push_back(MInstr::make_call_abs(out.intern_imm64(addr)));
                        /* 4. Resultado desde regs[0]. */
                        if (in.dst != ir::IR_NO_VALUE)
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                vm_reg_mem(0)));
                        break;
                    }

                    /* CALLVIRT: dispatch dinamico via vrt_callvirt(proc, obj,
                     * vtbl_idx).  Los args reales van a proc->registers; proc
                     * /obj/vtbl_idx van a arg_regs host; el resultado queda en
                     * regs[0]. */
                    case ir::IrOp::CALLVIRT: {
                        flush_pending();
                        if (!vm || callvirt_addr == 0) {
                            vreg_dbg(fn.name.c_str(), "callvirt(no-vm/no-addr)");
                            return false;
                        }
                        if (in.operands.empty()) return false;
                        const ir::IrValueId obj = in.operands[0];
                        const uint32_t vtbl_idx = static_cast<uint32_t>(in.imm);
                        /* 1. Stores de args reales a proc->registers.regs[i+1]
                         *    (operands[0]=obj=this -> regs[1], args -> regs[2..]). */
                        for (size_t i = 0; i < in.operands.size(); ++i)
                            O.push_back(MInstr::make_unary(MOp::MOV,
                                vm_reg_mem(static_cast<int>(i) + 1),
                                vr(in.operands[i])));
                        /* 2. Marshalling host: arg0=proc, arg1=obj, arg2=vtbl_idx.
                         *    obj PRIMERO (mientras vive en su reg). */
#if defined(_WIN32)
                        const MReg pr_reg = MReg::RCX, obj_reg = MReg::RDX, idx_reg = MReg::R8;
#else
                        const MReg pr_reg = MReg::RDI, obj_reg = MReg::RSI, idx_reg = MReg::RDX;
#endif
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(obj_reg, 8), vr(obj)));
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(pr_reg, 8), MOperand::make_reg(MReg::RBX, 8)));
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(idx_reg, 8),
                            MOperand::make_imm32(static_cast<int32_t>(vtbl_idx))));
                        /* 3. CALL vrt_callvirt. */
                        O.push_back(MInstr::make_call_abs(out.intern_imm64(callvirt_addr)));
                        /* 4. Resultado en regs[0]. */
                        if (in.dst != ir::IR_NO_VALUE)
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                vm_reg_mem(0)));
                        break;
                    }

                    default:
                        vreg_dbg(fn.name.c_str(), ir::ir_op_name(in.op));
                        return false;  // op fuera del subset -> fallback
                }
            }
            flush_pending();  // por si el bloque termina sin terminador explicito
            out.blocks.push_back(std::move(mb));
        }
        return true;
    }

} // namespace jit
