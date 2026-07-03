/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/machine_ir.h
 * @brief MachineIR target-specific para x86-64.
 *
 * = Diseno cache-friendly =
 *
 * Cada @c MInstr ocupa EXACTAMENTE 32 bytes -> 2 instrucciones por
 * cache line de 64 bytes.  Iterar un @c MBlock durante el encoder
 * pass es secuencial puro: 1 cache miss por cada 2 instrs en el
 * mejor caso.  El selector NO crea nodos en heap sueltos; usa
 * @c std::vector<MInstr> contiguo dentro de cada @c MBlock.
 *
 * = Operandos =
 *
 * @c MOperand es 8 bytes (uno por slot).  Para inmediatos de 64 bits
 * (mov r64, imm64), se almacena un indice a @c MFunction::imm64_pool;
 * el imm cabe en el value de la MOperand y la decodificacion se hace
 * en el encoder (zero overhead vs imm64 inline en cada MInstr).
 *
 * Para operandos de memoria @c MOperand encoda:
 *   - reg     = base register (0..63)
 *   - width   = scale (bits[1:0]: 1/2/4/8) + index_reg (bits[7:2])
 *   - flags   = address size hint (32-bit / 64-bit)
 *   - value   = disp32 (sign-extended)
 *
 * = Indices, no punteros =
 *
 * @c MBlock se referencia por @c uint16_t (max 65535 bloques por
 * funcion - mucho mas que suficiente).  Predecesores/sucesores se
 * almacenan en pools compartidos del @c MFunction para evitar
 * @c std::vector::vector overhead.
 *
 * = Convencion VM =
 *
 * El JIT-eado de VestaVM usa una convencion FIJA:
 *
 *   Entry de funcion JIT:
 *     - rdi (SysV) / rcx (Win64): ProcessVM *proc
 *     - args reales viven en @c proc->registers.regs[1..N]
 *     - return se escribe a @c proc->registers.regs[0]
 *     - argc en @c proc->registers.regs[15]
 *
 * El selector materializa los args bytecode-VM en regs nativos via
 * LOAD desde el offset apropiado de @c ProcessVM, y al final hace
 * STORE del return.  Toda la calling convention se maneja en el
 * selector, no en MachineIR.
 *
 * = Phases =
 *
 * Phase D.1.a (este header): tipos + lifetime.  NO emite bytes aun.
 * Phase D.1.b: instruction selector @c ssa_ir::IrFunction -> @c MFunction.
 * Phase D.1.c: encoder @c MFunction -> @c std::vector<uint8_t>.
 * Phase D.1.d: label resolution + relocations.
 */

#ifndef VESTA_JIT_MACHINE_IR_H
#define VESTA_JIT_MACHINE_IR_H

#include <cstdint>
#include <string>
#include <vector>

namespace jit {

/* ===================================================================== */
/* Registros x86-64                                                       */
/* ===================================================================== */

/**
 * @enum MReg
 * @brief Identificadores de registros maquina x86-64.
 *
 * Encoding compacto: los GP regs ocupan 0..15, los XMM 16..31.
 * El @c uint8_t en @c MOperand.reg los acepta directos.
 *
 * Mapeo VM -> host (convencion):
 *   - bytecode R0  -> RAX (return value)
 *   - bytecode R1  -> R10 (caller-saved scratch en SysV/Win64)
 *   - bytecode R2  -> R11
 *   - bytecode R3..R7  -> R8/R9 + 3 reservados de la VM
 *   - bytecode R15 -> R15 (argc, callee-saved)
 *
 * Reservados por el host ABI:
 *   - RSP/RBP: stack frame del codigo JIT
 *   - RDI (SysV) / RCX (Win64): @c ProcessVM* (primer arg)
 *
 * NOTE: la asignacion concreta VM-reg -> host-reg vive en el
 * selector.  MachineIR es agnostico; aqui solo definimos
 * el espacio de regs disponibles.
 */
/**
 * @enum RegClass
 * @brief Clase de un registro (virtual o fisico).  Phase D.7.
 *
 * El register allocator asigna cada clase de forma INDEPENDIENTE: un
 * vreg GP solo puede ir a un fisico GP, un vreg FP solo a un fisico FP.
 * Esto deja preparado el banco de coma flotante/vector (XMM en x86,
 * NEON en ARM) sin reescribir el core: añadir floats = registrar la
 * clase FP con sus fisicos.  En D.7 v1 solo se asignan registros GP;
 * los valores float siguen con memory-roundtrip hasta la fase XMM.
 */
enum class RegClass : uint8_t {
    GP =
        0, ///< proposito general: enteros y punteros (RAX..R15 / X0..X30 / ...)
    FP = 1,   ///< coma flotante / vector (XMM / NEON / ...) -- reservado en v1
    COUNT = 2 ///< numero de clases (para dimensionar tablas del TargetRegInfo)
};

/**
 * @enum AbiKind
 * @brief Convencion con la que se genera el prologue/epilogue y el paso de
 *        argumentos de una funcion JIT (Phase D.7).
 */
enum class AbiKind : uint8_t {
    HOST_LEAF = 0, ///< funcion hoja host: args en arg_regs, return en RAX
                   ///< (tests aislados).
    VM = 1         ///< VM_ABI: @c ProcessVM* en RCX(Win64)/RDI(SysV), args
                   ///< en @c proc->registers.regs[1..N], return en regs[0].
                   ///< RBX = @c ProcessVM* durante toda la funcion.
};

enum class MReg : uint8_t {
    /* GP 64-bit */
    RAX = 0,
    RCX = 1,
    RDX = 2,
    RBX = 3,
    RSP = 4,
    RBP = 5,
    RSI = 6,
    RDI = 7,
    R8 = 8,
    R9 = 9,
    R10 = 10,
    R11 = 11,
    R12 = 12,
    R13 = 13,
    R14 = 14,
    R15 = 15,
    /* XMM 128-bit (cubre f32/f64 escalar) */
    XMM0 = 16,
    XMM1 = 17,
    XMM2 = 18,
    XMM3 = 19,
    XMM4 = 20,
    XMM5 = 21,
    XMM6 = 22,
    XMM7 = 23,
    XMM8 = 24,
    XMM9 = 25,
    XMM10 = 26,
    XMM11 = 27,
    XMM12 = 28,
    XMM13 = 29,
    XMM14 = 30,
    XMM15 = 31,
    /* sentinela invalido */
    NONE = 63
};

/** @brief Convierte @c MReg a indice @c uint8_t para encoding. */
inline uint8_t reg_id(MReg r) noexcept {
    return static_cast<uint8_t>(r);
}

/** @brief True si @p r es un registro GP (0..15). */
inline bool is_gp(MReg r) noexcept {
    return reg_id(r) < 16;
}

/** @brief True si @p r es un registro XMM (16..31). */
inline bool is_xmm(MReg r) noexcept {
    const uint8_t id = reg_id(r);
    return id >= 16 && id < 32;
}

/* ===================================================================== */
/* Condiciones (para jcc / setcc / cmovcc)                                */
/* ===================================================================== */

/**
 * @enum MCond
 * @brief Codigos de condicion x86-64.  Los valores coinciden con la
 *        codificacion binaria del opcode (bits[3:0] de Jcc/SETcc).
 */
enum class MCond : uint8_t {
    O = 0x0,  ///< overflow
    NO = 0x1, ///< no overflow
    B = 0x2,  ///< below (CF=1)
    AE = 0x3, ///< above-or-equal (CF=0)
    E = 0x4,  ///< equal (ZF=1)
    NE = 0x5, ///< not equal (ZF=0)
    BE = 0x6, ///< below-or-equal
    A = 0x7,  ///< above
    S = 0x8,  ///< sign
    NS = 0x9, ///< not sign
    P = 0xA,  ///< parity
    NP = 0xB, ///< not parity
    L = 0xC,  ///< less (signed)
    GE = 0xD, ///< greater-or-equal (signed)
    LE = 0xE, ///< less-or-equal (signed)
    G = 0xF,  ///< greater (signed)
    NONE = 0xFF
};

/* ===================================================================== */
/* Operandos                                                              */
/* ===================================================================== */

/**
 * @enum MOperandKind
 * @brief Tipo del operando.
 */
enum class MOperandKind : uint8_t {
    NONE = 0,      ///< slot vacio
    REG = 1,       ///< registro maquina
    IMM32 = 2,     ///< inmediato 32-bit sign-extended
    IMM64_IDX = 3, ///< imm64 via indice a @c MFunction::imm64_pool
    MEM = 4,       ///< memoria: base + index*scale + disp32
    LABEL = 5,     ///< label_id (para JMP/JCC/CALL relativos)
    REL_RT = 6,    ///< runtime entry slot (puntero resuelto en link)
    VREG =
        7 ///< registro VIRTUAL (Phase D.7): id en @c value, clase en @c flags
};

/**
 * @struct MOperand
 * @brief Operando de instruccion maquina (8 bytes, cache-friendly).
 *
 * Layout:
 *   +0 [1] kind     MOperandKind
 *   +1 [1] reg      MReg (REG/MEM.base)
 *   +2 [1] width    bytes operand (1/2/4/8) o MEM.scale+index packed
 *   +3 [1] flags    reservado / mem_size override
 *   +4 [4] value    imm32 / label_id / disp32 / imm64_pool_idx
 *
 * Para @c MEM:
 *   - reg     = base register
 *   - width   = bits[1:0] scale (1/2/4/8 = 0/1/2/3),
 *               bits[7:2] index_reg (0..63), o 63 = none
 *   - flags   = address size (0=64-bit, 1=32-bit)
 *   - value   = disp32 signed
 */
struct MOperand {
    MOperandKind kind = MOperandKind::NONE;
    uint8_t reg = static_cast<uint8_t>(MReg::NONE);
    uint8_t width = 8; ///< bytes (1/2/4/8) o packed MEM scale|index
    uint8_t flags = 0;
    int32_t value = 0; ///< imm32 / label_id / disp / pool_idx

    /* Helpers de construccion (zero overhead, todos inline). */
    static MOperand none() noexcept { return MOperand{}; }

    static MOperand make_reg(MReg r, uint8_t w = 8) noexcept {
        MOperand o;
        o.kind = MOperandKind::REG;
        o.reg = reg_id(r);
        o.width = w;
        return o;
    }

    static MOperand make_imm32(int32_t v, uint8_t w = 4) noexcept {
        MOperand o;
        o.kind = MOperandKind::IMM32;
        o.width = w;
        o.value = v;
        return o;
    }

    static MOperand make_imm64_idx(uint32_t pool_idx) noexcept {
        MOperand o;
        o.kind = MOperandKind::IMM64_IDX;
        o.width = 8;
        o.value = static_cast<int32_t>(pool_idx);
        return o;
    }

    static MOperand make_mem(MReg base, int32_t disp, MReg index = MReg::NONE,
                             uint8_t scale = 1,
                             uint8_t addr_size_32 = 0) noexcept {
        MOperand o;
        o.kind = MOperandKind::MEM;
        o.reg = reg_id(base);
        /* scale: 1->0, 2->1, 4->2, 8->3 */
        uint8_t scale_bits = 0;
        switch (scale) {
        case 1: scale_bits = 0; break;
        case 2: scale_bits = 1; break;
        case 4: scale_bits = 2; break;
        case 8: scale_bits = 3; break;
        default: scale_bits = 0; break;
        }
        o.width =
            static_cast<uint8_t>((reg_id(index) << 2) | (scale_bits & 0x3));
        o.flags = addr_size_32 ? 1 : 0;
        o.value = disp;
        return o;
    }

    static MOperand make_label(uint32_t label_id) noexcept {
        MOperand o;
        o.kind = MOperandKind::LABEL;
        o.value = static_cast<int32_t>(label_id);
        return o;
    }

    /**
     * @brief Construye un operando de registro VIRTUAL (Phase D.7).
     *
     * El id del vreg vive en @c value (no en @c reg, que es u8 y se
     * reserva para fisicos 0..63).  La clase (GP/FP) se guarda en el
     * bit0 de @c flags.  @c width es el ancho del operando en bytes.
     *
     * @param vid    Id del registro virtual (denso 0..vreg_count-1).
     * @param cls    Clase del registro (GP/FP).
     * @param w      Ancho en bytes (1/2/4/8).
     */
    static MOperand make_vreg(uint32_t vid, RegClass cls,
                              uint8_t w = 8) noexcept {
        MOperand o;
        o.kind = MOperandKind::VREG;
        o.value = static_cast<int32_t>(vid);
        o.flags = static_cast<uint8_t>(cls);
        o.width = w;
        return o;
    }

    /* Accessors VREG */
    bool is_vreg() const noexcept { return kind == MOperandKind::VREG; }
    bool is_reg() const noexcept { return kind == MOperandKind::REG; }
    uint32_t vreg_id() const noexcept { return static_cast<uint32_t>(value); }
    RegClass vreg_class() const noexcept {
        return static_cast<RegClass>(flags & 0x1);
    }

    /* Accessors MEM */
    MReg mem_base() const noexcept { return static_cast<MReg>(reg); }
    MReg mem_index() const noexcept { return static_cast<MReg>(width >> 2); }
    uint8_t mem_scale() const noexcept {
        static const uint8_t S[4] = {1, 2, 4, 8};
        return S[width & 0x3];
    }
    int32_t mem_disp() const noexcept { return value; }
    bool mem_addr32() const noexcept { return (flags & 1) != 0; }
};

static_assert(sizeof(MOperand) == 8, "MOperand debe ser 8 bytes");

/* ===================================================================== */
/* Opcodes maquina (x86-64 v1)                                            */
/* ===================================================================== */

/**
 * @enum MOp
 * @brief Opcodes maquina abstractos.  El encoder los traduce a la
 *        secuencia de bytes x86-64 correspondiente.
 *
 * Cobertura v1: el subset minimo para que un C1
 * baseline JIT pueda compilar funciones de aritmetica + control
 * de flujo + calls.  Floats, SIMD y sync atomic se añaden en
 * fases posteriores (D.3+ para floats, D.8 para SIMD).
 */
enum class MOp : uint8_t {
    NOP = 0,

    /* Movimiento */
    MOV = 1,  ///< MOV dst, src (reg/imm/mem)
    LEA = 2,  ///< LEA dst, [mem]
    PUSH = 3, ///< PUSH src (reg/imm)
    POP = 4,  ///< POP dst (reg)

    /* ALU enteros */
    ADD = 10,
    SUB = 11,
    IMUL = 12, ///< IMUL dst, src (2-op signed)
    AND = 13,
    OR = 14,
    XOR = 15,
    SHL = 16, ///< SHL dst, imm8/cl
    SHR = 17,
    SAR = 18,
    NEG = 19,
    NOT = 20,
    IDIV = 21,  ///< IDIV src (RDX:RAX / src -> RAX, RDX = rem)
    CQO = 22,   ///< sign-extend RAX into RDX:RAX (CDQ for 32-bit)
    MOVZX = 23, ///< MOVZX dst, src (zero-extend u8/u16 -> u64)
    MOVSX = 24, ///< MOVSX dst, src (sign-extend i8/i16/i32 -> i64)
    INC = 25,   ///< INC dst (++dst, mas compacto que ADD dst, 1)
    DEC = 26,   ///< DEC dst (--dst, mas compacto que SUq dst, 1)

    /* Bit ops universales (Math-IR-promote v2.2a).
     * Cada uno es 1 instr nativa en x86 (BMI1/SSE4.2) y ARM/RISC-V. */
    POPCNT =
        27, ///< POPCNT dst, src (F3 0F B8 /r REX.W) -- count set bits (SSE4.2)
    LZCNT = 28, ///< LZCNT  dst, src (F3 0F BD /r REX.W) -- count leading zeros
                ///< (BMI1)
    TZCNT = 29, ///< TZCNT  dst, src (F3 0F BC /r REX.W) -- count trailing zeros
                ///< (BMI1)

    /* Comparacion + condicionales */
    CMP = 30,
    TEST = 31,
    SETCC = 32,  ///< variant=MCond
    CMOVCC = 33, ///< variant=MCond

    /* Control de flujo */
    JMP = 40,  ///< JMP rel32 (operand=LABEL)
    JCC = 41,  ///< Jcc rel32 (variant=MCond)
    CALL = 42, ///< CALL rel32 (LABEL) o reg
    RET = 43,
    INT3 = 44, ///< debug trap (0xCC)

    /* Pseudo-ops */
    LABEL_DEF = 50, ///< marca posicion de un label (no emite bytes)
    SAFEPOINT = 51, ///< polling check: cmp byte [rbx], 0 + jne handler
    COMMENT = 52,   ///< no-op con texto debug (skipped en release)
    CALL_ABS =
        53,    ///< CALL a direccion absoluta (via mov rax, imm64 + call rax)
    ARG = 54,  ///< pseudo (D.7): marca un argumento de la siguiente CALL.
               ///< variant = indice del arg; src1 = vreg.  No emite bytes;
               ///< el rewrite recolecta los ARG y hace el parallel-move a
               ///< los arg_regs justo antes del CALL.
    LOAD = 55, ///< pseudo (D.7 commit 7): dst = [addr_vreg] (disp 0).
               ///< dst = dst_vreg, src1 = addr_vreg; flags = (width<<1)|signed.
               ///< El rewrite lo baja a MOV/MOVZX/MOVSX con [reg].
    STORE = 56,  ///< pseudo (D.7 commit 7): [addr_vreg] = val_vreg (disp 0).
                 ///< src1 = addr_vreg, src2 = val_vreg; flags = width.
    ALLOCA = 57, ///< pseudo (D.7 commit 8): dst = host_ptr a @c value
                 ///< bytes reservados en el frame JIT.  dst = dst_vreg,
                 ///< value = size.  El rewrite lo baja a LEA [rbp-off].

    /* Bit ops adicionales (Math-IR-promote v2.2a, continuacion). */
    BSWAP = 60, ///< BSWAP dst (0F C8+rd REX.W) -- byte swap full register
    ROL = 61,   ///< ROL dst, cl/imm (D3/C1 /0 REX.W) -- rotate left
    ROR = 62,   ///< ROR dst, cl/imm (D3/C1 /1 REX.W) -- rotate right

    /* FP scalar ops (Math-IR-promote v2.2b).
     * Mov data entre GP regs y XMM regs via memory roundtrip o MOVQ
     * directo.  XMM regs estan FUERA del regalloc D.7 (que solo conoce
     * GP); las cases del Selector hardcodean XMM0/XMM1 como scratch. */
    MOVQ_GP_XMM = 63, ///< MOVQ xmm, rGP (66 REX.W 0F 6E /r) -- GP -> XMM
    SQRTSD = 65,      ///< SQRTSD xmm_dst, xmm_src (F2 0F 51 /r)
    MINSD = 66, ///< MINSD  xmm_dst, xmm_src (F2 0F 5D /r) -- NaN: returns src2
    MAXSD = 67, ///< MAXSD  xmm_dst, xmm_src (F2 0F 5F /r)
    ROUNDSD = 68, ///< ROUNDSD xmm_dst, xmm_src, imm8 (66 0F 3A 0B /r ib)
                  ///<   imm8 modes: 0=round-to-nearest, 1=floor, 2=ceil,
                  ///<   3=trunc. variant field carga el mode.  Requiere SSE4.1
                  ///<   (universal x86-64).
    MOVQ_XMM_GP = 69, ///< MOVQ rGP, xmm (66 REX.W 0F 7E /r) -- XMM -> GP

    /* FP arith scalar (Math-IR-promote v2.2b cont). */
    ADDSD = 70, ///< ADDSD xmm_dst, xmm_src (F2 0F 58 /r) -- f64 add
    SUBSD = 71, ///< SUBSD xmm_dst, xmm_src (F2 0F 5C /r) -- f64 sub
    MULSD = 72, ///< MULSD xmm_dst, xmm_src (F2 0F 59 /r) -- f64 mul
    DIVSD = 73, ///< DIVSD xmm_dst, xmm_src (F2 0F 5E /r) -- f64 div
    CVTSI2SD =
        74, ///< CVTSI2SD xmm, r64 (F2 REX.W 0F 2A /r) -- int signed -> f64
    CVTTSD2SI =
        75, ///< CVTTSD2SI r64, xmm (F2 REX.W 0F 2C /r) -- f64 -> int truncado
    UCOMISD =
        76, ///< UCOMISD xmm_a, xmm_b (66 0F 2E /r) -- f64 compare, set ZF/PF/CF
    CVTSS2SD = 77, ///< CVTSS2SD xmm, xmm (F3 0F 5A /r) -- f32 -> f64 (widen)
    CVTSD2SS = 78, ///< CVTSD2SS xmm, xmm (F2 0F 5A /r) -- f64 -> f32 (narrow)

    /* Pseudo D.7 perf (2026-06-06): division/modulo entero en vregs.
     * dst = src1 / src2 (variant 0 = DIV, 1 = MOD).  El rewrite lo
     * expande a la secuencia x86 RAX:RDX-fija usando R11 (scratch
     * reservado) para el divisor, evitando el aliasing operando<->RAX/RDX
     * sin necesitar fixed intervals en el regalloc.  Se marca como
     * call-position (clobber RAX/RDX -> live-across van a callee-saved). */
    DIVMOD_V = 79,

    /* Pseudo (callback-ABI 2026-06-06): carga el @c ProcessVM* del
     * proceso actual en @c dst.reg (siempre RBX).  Encapsula la
     * decision TLS-direct-vs-call que el thunk hacia a mano:
     *   - src1.value != -1: @c mov dst, gs:[src1.value]  (TLS-direct
     *     Win64; src1.value = 0x1480 + tls_idx*8).
     *   - src1.value == -1: @c mov rax, imm64(get_proc) ; call rax ;
     *     mov dst, rax  (fallback portable; la direccion de
     *     @c get_current_executing_process vive en
     *     @c imm64_pool[src2.value]).
     * El selector resuelve TLS/addr (puede llamar al runtime) y los
     * hornea aqui; el encoder solo emite bytes -> sin dependencia
     * del runtime en el encoder. */
    LOAD_PROC = 80,

    /* Pseudo (cobertura vreg vm_mem, 2026-06-09): acceso a memoria del VM
     * (vaddr, is_host_ptr=false).  La memoria del VM NO es contigua (TLB
     * 3-niveles + arenas dispersas) -> no hay traduccion base+offset.  El
     * rewrite los expande POST-regalloc al patron page-cache INLINE del
     * selector (cmp contra la pagina cacheada + load/store directo en hit;
     * CALL a vrt_vm_read/write_u<w> en miss).  Marcados call-position (el
     * miss clobbea caller-saved).  La direccion de la funcion de fallback
     * se hornea en imm64_pool y su indice viaja en un operando libre del
     * MInstr (src2 para LOAD_VM, dst para STORE_VM). */
    LOAD_VM = 82,   ///< dst = vm_mem[addr] (page-cache inline + fallback).
                    ///< dst = dst_vreg, src1 = addr_vreg,
                    ///< src2 = imm64_idx(&vm_read_u<w>);
                    ///< flags = (width<<1)|signed.
    STORE_VM = 83,  ///< vm_mem[addr] = val (page-cache inline + fallback).
                    ///< src1 = addr_vreg, src2 = val_vreg,
                    ///< dst = imm64_idx(&vm_write_u<w>); flags = width.
    ALLOCA_VM = 84, ///< Fase 2: dst = vaddr a `size` bytes reservados en
                    ///< el VM stack del proceso (proc->stack_pointer).
                    ///< dst = dst_vreg, src1 = imm32(size).  El
                    ///< prologue salva el VM-RSP y el epilogue lo
                    ///< restaura (regalloc_rewrite).  El dst es un
                    ///< vaddr (is_host_ptr=false) -> sus LOAD/STORE
                    ///< van por LOAD_VM/STORE_VM (page-cache).

    /* Pseudo TCO (2026-06-10): tail-call con REUSO de frame.  El rewrite
     * lo expande POST-regalloc a: mov A0,rbx (proc -> arg0, sobrevive el
     * teardown por ser caller-saved) + emit_epilogue (desmonta el frame
     * actual; rsp queda apuntando a la return address del caller) + jmp al
     * target (code+0 del callee).  El prologue del callee monta un frame
     * fresco; su RET retorna al caller original -> profundidad de pila O(1)
     * (igual que el bytecode tailcall 0x24).  src1 = LABEL(bloque 0) para
     * self-tail-call; src1 = imm64_idx(addr) para tail-call cross-fn. */
    TAILCALL = 85,

    /* Pseudo (Phase AS inc.5): bloque de inline-asm nativo.  src1 =
     * IMM32(blob_idx) -> indice en @c MFunction::asm_blobs.  El encoder
     * apendea los bytes ya ensamblados (via @c vex::g_asm_backend) verbatim
     * al code cache.  No tiene operandos vreg propios: los inputs/outputs
     * register-bound viven en sus registros fisicos ANTES/DESPUES (pineados
     * por el regalloc via @c MFunction::vreg_fixed).  Para la liveness, el
     * @c AsmBlob asociado lista los vregs leidos/escritos por el asm + los
     * registros clobbered; @c build_intervals los trata como use/def en esta
     * posicion (ver 5c). */
    INLINE_ASM_RAW = 86,

    /* Pseudo AOT (Phase AOT.3 Paso 2b-ii): referencias a simbolos que el
     * encoder emite con un placeholder + una @c MReloc, y que el driver
     * parchea tras el layout de @c .text/.rodata.  Solo se generan en el
     * codegen AOT (HOST_LEAF standalone); el JIT en proceso resuelve las
     * direcciones en compile-time y usa CALL_ABS / MOV imm64 directos. */
    CALL_SYM = 87, ///< CALL rel32 a una funcion del modulo por NOMBRE.
                   ///< src1 = IMM32(sym_idx -> MFunction::reloc_symbols).
                   ///< El encoder emite E8 + rel32=0 + MReloc{CALL_REL32}.
    MOV_SYM = 88,  ///< mov dst, &simbolo (direccion absoluta 64-bit de un
                   ///< dato de .rodata).  dst = reg/vreg, src1 =
                   ///< IMM32(sym_idx).  El encoder emite REX.W B8+rd +
                   ///< imm64=0 + MReloc{ABS64}.
    JMP_SYM = 89,  ///< JMP rel32 a una funcion del modulo por NOMBRE
                   ///< (tail-call TCO: epilogue + jmp al callee).  src1 =
                   ///< IMM32(sym_idx).  El encoder emite E9 + rel32=0 +
                   ///< MReloc{CALL_REL32} (misma matematica rel32).
    LEA_RIP_SYM =
        90, ///< lea dst, [rip+disp32] -> direccion de un DATO de
            ///< .rodata por NOMBRE (ref position-independent, default
            ///< AOT).  dst = reg/vreg, src1 = IMM32(sym_idx).  El
            ///< encoder emite 48 8D 05 + disp32=0 + MReloc{DATA_REL32}.
    LEA_LABEL =
        91, ///< lea dst, [rip+disp32] -> direccion NATIVA de un LABEL local
            ///< (intra-funcion).  dst = reg/vreg, src1 = LABEL(label_id).  El
            ///< encoder emite 48 8D 05 + disp32=0 + MFixup{label} (mismo
            ///< rel32 que jmp/jcc: disp = label_off - instr_end).  Usado por
            ///< TRYENTER in-JIT para capturar la direccion del bloque catch.

    TLS_LE_ADDR =
        159, ///< dst = direccion por-hilo de un `thread_local` (TLS local-exec,
            ///< ELF).  src1 = IMM32(sym_idx) del simbolo TLS.  El encoder emite
            ///< `mov dst, %fs:0` (64 REX.W 8B + SIB disp32=0) + `lea dst,
            ///< [dst+disp32]` con una @c MReloc{TPOFF32} sobre el disp32 del
            ///< lea.  El resultado es un host_ptr (TP + tpoff).

    TLS_PE_ADDR =
        160, ///< dst = direccion por-hilo de un `thread_local` (TLS PE/Windows).
             ///< src1 = IMM32(var_sym_idx) del simbolo .tls; src2 =
             ///< IMM32(index_sym_idx) del `__vex_tls_index`.  El encoder emite
             ///< `mov r10,gs:[0x58]` + `mov r11d,[rip+_tls_index]` (DATA_REL32)
             ///< + `mov r10,[r10+r11*8]` + `lea dst,[r10+var@secrel]`
             ///< (SECREL32).  Usa r10/r11 (scratch reservados) -> dst libre.

    /* FP-regalloc (Phase AOT C1 float, 2026-06-17): movimiento de datos
     * f64/f32 entre XMM regs y entre XMM y memoria (spills, param-load/store,
     * float CONST).  A diferencia de ADDSD/etc (reg-reg only), MOVSD/MOVSS
     * aceptan un operando de memoria -> el rewrite los usa para materializar
     * spills FP y el two-address legalization (mov dst,src antes de la op).
     * El encoder distingue la direccion por el kind de dst/src: xmm<-xmm o
     * xmm<-mem (F2/F3 0F 10), mem<-xmm (F2/F3 0F 11). */
    MOVSD = 92, ///< MOVSD dst, src (f64): F2 0F 10 (load) / F2 0F 11 (store)
    MOVSS = 93, ///< MOVSS dst, src (f32): F3 0F 10 / F3 0F 11

    /* FP arith scalar f32 (SSE, prefijo F3).  Mismo patron 2-address que
     * sus contrapartes f64 (ADDSD/...): el selector emite forma 3-op y el
     * rewrite la legaliza.  Reg-reg only (igual que ADDSD). */
    ADDSS = 94,     ///< ADDSS xmm,xmm (F3 0F 58)
    SUBSS = 95,     ///< SUBSS xmm,xmm (F3 0F 5C)
    MULSS = 96,     ///< MULSS xmm,xmm (F3 0F 59)
    DIVSS = 97,     ///< DIVSS xmm,xmm (F3 0F 5E)
    SQRTSS = 98,    ///< SQRTSS xmm,xmm (F3 0F 51)
    UCOMISS = 99,   ///< UCOMISS xmm,xmm (0F 2E) -- f32 compare (sin 66)
    CVTSI2SS = 100, ///< CVTSI2SS xmm,r64 (F3 REX.W 0F 2A) -- int -> f32
    CVTTSS2SI =
        101,      ///< CVTTSS2SI r64,xmm (F3 REX.W 0F 2C) -- f32 -> int trunc
    XORPS = 102,  ///< XORPS xmm,xmm (0F 57) -- clear / neg-mask de f32/f64
    ANDPS = 103,  ///< ANDPS xmm,xmm (0F 54) -- abs-mask de f32/f64 (FABS)

    /* memcpy x86 nativo (perf strings 2026-06-18).  REP MOVSB copia RCX
     * bytes desde [RSI] a [RDI] incrementando ambos (DF=0 asumido por la
     * ABI host).  Es la instruccion de copia mas rapida del CPU moderno
     * (fast-string-ops / ERMSB).  No tiene operandos vreg propios: opera
     * sobre RSI/RDI/RCX FIJOS, que el selector carga (y salva/restaura con
     * PUSH/POP) en la secuencia auto-contenida del IrOp::MEMCPY.  El
     * encoder emite los 2 bytes F3 A4.  Indep. del regalloc (no asigna ni
     * clobbea vregs porque la save/restore preserva RSI/RDI/RCX). */
    REP_MOVSB = 104,

    /* Packed FP SSE2 (auto-vectorizacion, 2026-06-25): operan sobre 2x f64
     * (128-bit XMM).  Prefijo 66 (packed-double).  Reg-reg (arith) o reg-mem
     * (MOVUPD/MOVAPD).  Base de la vectorizacion de loops float y, a futuro,
     * de los tipos anchos N=potencia-de-2 (i128 -> XMM completo).  AVX (VEX,
     * 4x f64) y AVX512 (EVEX, 8x f64) son slices posteriores con el mismo
     * patron pero distinto encoding. */
    ADDPD = 106,  ///< ADDPD xmm,xmm (66 0F 58) -- 2x f64 add
    SUBPD = 107,  ///< SUBPD xmm,xmm (66 0F 5C) -- 2x f64 sub
    MULPD = 108,  ///< MULPD xmm,xmm (66 0F 59) -- 2x f64 mul
    DIVPD = 109,  ///< DIVPD xmm,xmm (66 0F 5E) -- 2x f64 div
    MOVUPD = 110, ///< MOVUPD dst,src (66 0F 10 load / 66 0F 11 store) 16B unaligned
    MOVAPD = 111, ///< MOVAPD dst,src (66 0F 28 load / 66 0F 29 store) 16B aligned

    /* Packed ENTEROS SSE2 (auto-vectorizacion de loops int).  Mismo patron que
     * los packed float (66 0F xx, reg-reg) pero suma/resta entera por lane.
     * No hay div entero packed en SSE (los loops int div/mul-i64 caen a
     * escalar).  PADDD/PSUBD = 4x i32; PADDQ/PSUBQ = 2x i64. */
    PADDD = 124, ///< PADDD xmm,xmm (66 0F FE) -- 4x i32 add
    PSUBD = 125, ///< PSUBD xmm,xmm (66 0F FA) -- 4x i32 sub
    PADDQ = 126, ///< PADDQ xmm,xmm (66 0F D4) -- 2x i64 add
    PSUBQ = 127, ///< PSUBQ xmm,xmm (66 0F FB) -- 2x i64 sub
    /* Packed 16-bit (word) y 8-bit (byte) SSE2.  WIG (W ignorado).  El word
     * tiene mul packed (PMULLW), el byte NO.  PADDW=8x i16; PADDB=16x i8. */
    PADDW = 143,  ///< PADDW xmm,xmm  (66 0F FD) -- 8x i16 add
    PSUBW = 144,  ///< PSUBW xmm,xmm  (66 0F F9) -- 8x i16 sub
    PMULLW = 145, ///< PMULLW xmm,xmm (66 0F D5) -- 8x i16 mul (low 16b)
    PADDB = 146,  ///< PADDB xmm,xmm  (66 0F FC) -- 16x i8 add
    PSUBB = 147,  ///< PSUBB xmm,xmm  (66 0F F8) -- 16x i8 sub
    /* Packed 32-bit mul (low): SSE4.1 / AVX2.  Mapa 0F38 (no 0F).  Permite
     * vectorizar `c[i]=a[i]*b[i]` para i32/u32 (PADDD/PSUBD ya cubren add/sub). */
    PMULLD = 148, ///< PMULLD xmm,xmm (66 0F38 40) -- 4x i32 mul (low 32b)

    /* AVX escalar 3-OPERANDOS no-destructivo (VEX.LIG.F2/F3.0F): VADDSD dst,
     * src1, src2 -> dst = src1 OP src2 (dst != src1 permitido).  A diferencia de
     * ADDSD (2-address destructivo, el rewrite mete un `mov dst,src1`), estas
     * NO necesitan ese mov -> el regalloc/scheduler las explota como 3-op
     * first-class.  Las emite el selector cuando --float-isa >= AVX; el src2
     * puede ser MEM (VEX reg-reg-mem).  SD = F2 (double), SS = F3 (single). */
    VADDSD = 149, ///< VADDSD dst,src1,src2/mem (VEX.LIG.F2.0F 58) -- f64 add
    VSUBSD = 150, ///< VSUBSD (VEX.LIG.F2.0F 5C) -- f64 sub
    VMULSD = 151, ///< VMULSD (VEX.LIG.F2.0F 59) -- f64 mul
    VDIVSD = 152, ///< VDIVSD (VEX.LIG.F2.0F 5E) -- f64 div
    VADDSS = 153, ///< VADDSS (VEX.LIG.F3.0F 58) -- f32 add
    VSUBSS = 154, ///< VSUBSS (VEX.LIG.F3.0F 5C) -- f32 sub
    VMULSS = 155, ///< VMULSS (VEX.LIG.F3.0F 59) -- f32 mul
    VDIVSS = 156, ///< VDIVSS (VEX.LIG.F3.0F 5E) -- f32 div
    /* VEX 3-op de XORPS/ANDPS (NP.0F 57/54): FNEG/FABS escalar en avx (dst =
     * src XOR/AND mascara-de-signo).  3-operandos no-destructivo, sin el `mov`
     * 2-address y sin legacy SSE mezclado con el resto VEX. */
    VXORPS = 157, ///< VXORPS dst,src1,src2 (VEX.LIG.NP.0F 57) -- fneg
    VANDPS = 158, ///< VANDPS dst,src1,src2 (VEX.LIG.NP.0F 54) -- fabs

    /* Packed FP unarios SSE2 (auto-vectorizacion de loops `b[i] = OP a[i]`):
     * SQRTPD (sqrt por lane), XORPD/ANDPD (fneg/fabs via mascara de signo) y
     * UNPCKLPD (difunde el lane bajo a ambos -> construye la mascara de signo
     * de 16B desde un MOVQ_GP_XMM, sin constante en memoria).  Mismo form
     * simple 66 0F xx /r reg-reg que los demas packed. */
    SQRTPD = 128,   ///< SQRTPD xmm,xmm   (66 0F 51) -- 2x f64 sqrt
    XORPD = 129,    ///< XORPD xmm,xmm    (66 0F 57) -- xor 128b (fneg via mask)
    ANDPD = 130,    ///< ANDPD xmm,xmm    (66 0F 54) -- and 128b (fabs via mask)
    UNPCKLPD = 131, ///< UNPCKLPD xmm,xmm (66 0F 14) -- dst.hi = src.lo (broadcast)
    /* Broadcast de un f64 (xmm.lo) a TODOS los lanes de un YMM/ZMM (mapa 0F38).
     * Solo AVX (VEX.256.66.0F38.W0 19 / EVEX.512.66.0F38.W1 19); para 128b se
     * usa UNPCKLPD.  Construye la mascara de signo wide de fneg/fabs. */
    VBROADCASTSD = 132, ///< VBROADCASTSD ymm/zmm, xmm

    /* Packed SINGLE (f32): mismos opcodes 0F 58/5C/59/5E/51/57/54 que los PD
     * pero SIN el prefijo 66 (pp=00 en VEX/EVEX; EVEX W0).  4x f32 (XMM),
     * 8x (YMM), 16x (ZMM).  Para mover bytes se reusa MOVUPD (da igual el
     * prefijo en un move crudo). */
    ADDPS = 133,        ///< ADDPS xmm,xmm  (0F 58) -- f32 add
    SUBPS = 134,        ///< SUBPS xmm,xmm  (0F 5C)
    MULPS = 135,        ///< MULPS xmm,xmm  (0F 59)
    DIVPS = 136,        ///< DIVPS xmm,xmm  (0F 5E)
    /* FMA fusionado (dot-product): dst = src1*src2 + dst (1 redondeo).  src2
     * (rm) puede ser memoria.  VFMADD231PD (66 0F38 W1 B8) -> f64; VFMADD231PS
     * (66 0F38 W0 B8) -> f32.  Solo AVX/AVX512 (no hay FMA en SSE2 base). */
    VFMADD231PD = 141,  ///< VFMADD231PD dst, src1, src2/mem (f64)
    VFMADD231PS = 142,  ///< VFMADD231PS dst, src1, src2/mem (f32)

    DATA_PTR_LABEL = 112, ///< Entrada de 8 bytes de la jump table densa:
                          ///< emite 8 zeros + registra un AddrTableFixup
                          ///< {offset, src1=LABEL}.  El pipeline lo parchea
                          ///< POST-memcpy con base + label_offsets[label].
                          ///< No es codigo ejecutable (se salta); el dispatch
                          ///< lo lee via `mov rT, [rbase + idx*8]`.

    DATA_REL32_LABEL = 113, ///< Entrada de 4 bytes de jump table SELF-RELATIVE
                            ///< (AOT/HOST_LEAF, PIC-safe, SIN reloc): emite 4
                            ///< zeros + MFixup{label=src1(block),
                            ///< instr_end=offset[src2(table)]} -> resolve_fixups
                            ///< escribe offset[block]-offset[table].  El
                            ///< dispatch suma la base: lea RB,[rip+table];
                            ///< movsxd RI,[RB+idx*4]; add RB,RI; jmp RB.

    COUNT = 114
};

/* ===================================================================== */
/* MInstr (32 bytes, 2 por cache line)                                    */
/* ===================================================================== */

/**
 * @struct MInstr
 * @brief Instruccion maquina.  EXACTAMENTE 32 bytes para cache locality.
 *
 * Layout:
 *   +0  [1]  op           MOp
 *   +1  [1]  variant      sub-tipo (MCond para JCC/SETCC, etc.)
 *   +2  [2]  flags        flags de encoding (rex.W override, prefix...)
 *   +4  [4]  source_pc    bytecode_pc original (debug)
 *   +8  [8]  dst          MOperand
 *   +16 [8]  src1         MOperand
 *   +24 [8]  src2         MOperand (para 3-operand ALU)
 */
/* Bit de @c MInstr::flags: emitir la op escalar float en VEX (avx+) en vez de
 * legacy SSE.  Lo pone el selector en cvt/cmp/sqrt cuando --float-isa>=AVX, para
 * NO mezclar legacy-SSE con las binarias VEX (penalizacion de transicion).  Para
 * las binarias arith hay MOps VEX dedicadas (VADDSD...); estas ops 1-fuente no
 * ganan nada de 3-op, asi que el flag es lo economico (no se inventa una MOp por
 * cada una).  Bit alto -> no colisiona con el stackmap-idx que CALL/SAFEPOINT
 * guardan en flags (esas no son ops float). */
static constexpr uint16_t MI_FLAG_VEX_SCALAR = 0x8000u;

struct MInstr {
    MOp op = MOp::NOP;
    uint8_t variant = 0;
    uint16_t flags = 0;
    uint32_t source_pc = 0;
    /* Solo-LSP (correlacion IR<->asm exacta): identidad estable de la op IR
     * origen = block_index*65536 + instr_pos (UINT32_MAX = sintetica, p.ej.
     * copias de PHI).  El inspector la decodifica a fn.blocks[bi].instrs[pos]. */
    uint32_t ir_id = 0xFFFFFFFFu;
    MOperand dst;
    MOperand src1;
    MOperand src2;

    /* Helpers de construccion (zero overhead). */
    static MInstr make_nop() noexcept {
        MInstr i;
        return i;
    }

    static MInstr make_unary(MOp op, MOperand dst, MOperand src) noexcept {
        MInstr i;
        i.op = op;
        i.dst = dst;
        i.src1 = src;
        return i;
    }

    static MInstr make_binary(MOp op, MOperand dst, MOperand a,
                              MOperand b) noexcept {
        MInstr i;
        i.op = op;
        i.dst = dst;
        i.src1 = a;
        i.src2 = b;
        return i;
    }

    static MInstr make_ret() noexcept {
        MInstr i;
        i.op = MOp::RET;
        return i;
    }

    /** @brief REP MOVSB: copia RCX bytes [RSI]->[RDI].  Sin operandos
     *  vreg (opera sobre fisicos fijos; el selector los carga/restaura). */
    static MInstr make_rep_movsb() noexcept {
        MInstr i;
        i.op = MOp::REP_MOVSB;
        return i;
    }

    static MInstr make_jmp(uint32_t label_id) noexcept {
        MInstr i;
        i.op = MOp::JMP;
        i.src1 = MOperand::make_label(label_id);
        return i;
    }

    static MInstr make_jcc(MCond cc, uint32_t label_id) noexcept {
        MInstr i;
        i.op = MOp::JCC;
        i.variant = static_cast<uint8_t>(cc);
        i.src1 = MOperand::make_label(label_id);
        return i;
    }

    static MInstr make_call_label(uint32_t label_id) noexcept {
        MInstr i;
        i.op = MOp::CALL;
        i.src1 = MOperand::make_label(label_id);
        return i;
    }

    /** @brief Pseudo ARG: argumento @p idx de la siguiente CALL (D.7). */
    static MInstr make_arg(uint8_t idx, MOperand src) noexcept {
        MInstr i;
        i.op = MOp::ARG;
        i.variant = idx;
        i.src1 = src;
        return i;
    }

    /** @brief CALL a direccion absoluta (en @c imm64_pool[@p imm64_idx]). */
    static MInstr make_call_abs(uint32_t imm64_idx) noexcept {
        MInstr i;
        i.op = MOp::CALL_ABS;
        i.src1 = MOperand::make_imm64_idx(imm64_idx);
        return i;
    }

    /** @brief TAILCALL self: reuso de frame, salto rel32 a code+0 (label
     *  del bloque 0).  El rewrite emite epilogue + jmp label. */
    static MInstr make_tailcall_label(uint32_t label_id) noexcept {
        MInstr i;
        i.op = MOp::TAILCALL;
        i.src1 = MOperand::make_label(label_id);
        return i;
    }

    /** @brief TAILCALL cross-fn: reuso de frame, salto a una direccion
     *  absoluta (en @c imm64_pool[@p imm64_idx]).  El rewrite emite
     *  epilogue + mov scratch,addr + jmp scratch. */
    static MInstr make_tailcall_abs(uint32_t imm64_idx) noexcept {
        MInstr i;
        i.op = MOp::TAILCALL;
        i.src1 = MOperand::make_imm64_idx(imm64_idx);
        return i;
    }

    /**
     * @brief Pseudo LOAD_PROC: carga @c ProcessVM* en @p dst (RBX).
     * @param dst            Registro destino (RBX por convencion).
     * @param tls_gs_disp    Desplazamiento @c gs:[disp] para TLS-direct
     *                       (Win64), o -1 para usar el fallback por call.
     * @param getproc_pool_idx  Indice en @c imm64_pool de la direccion de
     *                       @c get_current_executing_process (usado solo
     *                       en el fallback).
     */
    static MInstr make_load_proc(MReg dst, int32_t tls_gs_disp,
                                 uint32_t getproc_pool_idx) noexcept {
        MInstr i;
        i.op = MOp::LOAD_PROC;
        i.dst = MOperand::make_reg(dst);
        i.src1 = MOperand::make_imm32(tls_gs_disp);
        i.src2 = MOperand::make_imm64_idx(getproc_pool_idx);
        return i;
    }

    /** @brief LOAD: @p dst = [@p addr] (host memory, disp 0).  @p width =
     *  1/2/4/8 bytes; @p sgn = sign-extend (i*) vs zero-extend (u*). */
    static MInstr make_load(MOperand dst, MOperand addr, uint8_t width,
                            bool sgn) noexcept {
        MInstr i;
        i.op = MOp::LOAD;
        i.dst = dst;
        i.src1 = addr;
        i.flags = static_cast<uint16_t>((width << 1) | (sgn ? 1u : 0u));
        return i;
    }
    /** @brief STORE: [@p addr] = @p val (host memory, disp 0). */
    static MInstr make_store(MOperand addr, MOperand val,
                             uint8_t width) noexcept {
        MInstr i;
        i.op = MOp::STORE;
        i.src1 = addr;
        i.src2 = val;
        i.flags = width;
        return i;
    }
    /** @brief LOAD_VM: @p dst = vm_mem[@p addr] (memoria del VM, vaddr).
     *  @p width = 1/2/4/8; @p sgn = sign-extend.  @p fn_idx = indice en
     *  @c imm64_pool de la direccion de @c vrt_vm_read_u<width> (usada en
     *  el fallback page-miss).  El rewrite lo expande al page-cache inline. */
    static MInstr make_load_vm(MOperand dst, MOperand addr, uint8_t width,
                               bool sgn, uint32_t fn_idx) noexcept {
        MInstr i;
        i.op = MOp::LOAD_VM;
        i.dst = dst;
        i.src1 = addr;
        i.src2 = MOperand::make_imm64_idx(fn_idx);
        i.flags = static_cast<uint16_t>((width << 1) | (sgn ? 1u : 0u));
        return i;
    }
    /** @brief STORE_VM: vm_mem[@p addr] = @p val (memoria del VM, vaddr).
     *  @p width = 1/2/4/8.  @p fn_idx = indice en @c imm64_pool de la
     *  direccion de @c vrt_vm_write_u<width> (usada en el fallback). */
    static MInstr make_store_vm(MOperand addr, MOperand val, uint8_t width,
                                uint32_t fn_idx) noexcept {
        MInstr i;
        i.op = MOp::STORE_VM;
        i.src1 = addr;
        i.src2 = val;
        i.dst = MOperand::make_imm64_idx(fn_idx);
        i.flags = width;
        return i;
    }
    /** @brief ALLOCA: @p dst = host_ptr a @p size bytes del frame JIT. */
    static MInstr make_alloca(MOperand dst, uint32_t size) noexcept {
        MInstr i;
        i.op = MOp::ALLOCA;
        i.dst = dst;
        i.src1 = MOperand::make_imm32(static_cast<int32_t>(size));
        return i;
    }
    /** @brief ALLOCA_VM: @p dst = vaddr a @p size bytes del VM stack. */
    static MInstr make_alloca_vm(MOperand dst, uint32_t size) noexcept {
        MInstr i;
        i.op = MOp::ALLOCA_VM;
        i.dst = dst;
        i.src1 = MOperand::make_imm32(static_cast<int32_t>(size));
        return i;
    }

    static MInstr make_label_def(uint32_t label_id) noexcept {
        MInstr i;
        i.op = MOp::LABEL_DEF;
        i.src1 = MOperand::make_label(label_id);
        return i;
    }

    /**
     * @brief Crea un MInstr SAFEPOINT.  El encoder lo expande a la
     *        secuencia de poll x86-64 documentada en @c MOp::SAFEPOINT.
     *
     * @c handler_imm64_idx es el indice en @c MFunction::imm64_pool
     * que contiene la direccion absoluta de @c vrt_safepoint_handler
     * (resuelta por el Selector via @c RuntimeEntries::safepoint_handler).
     */
    static MInstr make_safepoint(uint32_t handler_imm64_idx) noexcept {
        MInstr i;
        i.op = MOp::SAFEPOINT;
        i.src1 = MOperand::make_imm64_idx(handler_imm64_idx);
        return i;
    }

    /** @brief INLINE_ASM_RAW: bloque de inline-asm nativo (Phase AS inc.5).
     *  @p blob_idx = indice en @c MFunction::asm_blobs con los bytes ya
     *  ensamblados + la info de liveness/clobbers.  El idx viaja como IMM32
     *  en @c src1 (no es un vreg). */
    static MInstr make_inline_asm_raw(uint32_t blob_idx) noexcept {
        MInstr i;
        i.op = MOp::INLINE_ASM_RAW;
        i.src1 = MOperand::make_imm32(static_cast<int32_t>(blob_idx));
        return i;
    }

    /** @brief CALL_SYM: CALL rel32 a la funcion del modulo @p sym_idx
     *  (indice en @c MFunction::reloc_symbols).  El encoder deja un rel32
     *  placeholder + una @c MReloc{CALL_REL32}. */
    static MInstr make_call_sym(uint32_t sym_idx) noexcept {
        MInstr i;
        i.op = MOp::CALL_SYM;
        i.src1 = MOperand::make_imm32(static_cast<int32_t>(sym_idx));
        return i;
    }

    /** @brief MOV_SYM: @p dst = &simbolo @p sym_idx (direccion absoluta de
     *  un dato de @c .rodata).  El encoder emite mov r64,imm64 placeholder +
     *  una @c MReloc{ABS64}. */
    static MInstr make_mov_sym(MOperand dst, uint32_t sym_idx) noexcept {
        MInstr i;
        i.op = MOp::MOV_SYM;
        i.dst = dst;
        i.src1 = MOperand::make_imm32(static_cast<int32_t>(sym_idx));
        return i;
    }

    /** @brief TAILCALL a una funcion del modulo por nombre (AOT HOST_LEAF):
     *  marca el sym_idx en @c src2 (IMM32) -- @c src1 queda NONE para
     *  distinguirla de las variantes label (self) / abs (cross-fn VM).  El
     *  rewrite hace parallel-move de args + epilogue + JMP_SYM (TCO real). */
    static MInstr make_tailcall_sym(uint32_t sym_idx) noexcept {
        MInstr i;
        i.op = MOp::TAILCALL;
        i.src2 = MOperand::make_imm32(static_cast<int32_t>(sym_idx));
        return i;
    }

    /** @brief JMP_SYM: jmp rel32 a la funcion del modulo @p sym_idx (cola del
     *  tail-call AOT).  El encoder deja E9 + rel32 placeholder + MReloc. */
    static MInstr make_jmp_sym(uint32_t sym_idx) noexcept {
        MInstr i;
        i.op = MOp::JMP_SYM;
        i.src1 = MOperand::make_imm32(static_cast<int32_t>(sym_idx));
        return i;
    }

    /** @brief LEA_RIP_SYM: @p dst = &dato @p sym_idx (RIP-relativo,
     *  position-independent).  El encoder emite lea reg,[rip+disp32=0] +
     *  una @c MReloc{DATA_REL32}. */
    static MInstr make_lea_rip_sym(MOperand dst, uint32_t sym_idx) noexcept {
        MInstr i;
        i.op = MOp::LEA_RIP_SYM;
        i.dst = dst;
        i.src1 = MOperand::make_imm32(static_cast<int32_t>(sym_idx));
        return i;
    }

    /** @brief LEA_LABEL: @p dst = direccion nativa del @p label_id local
     *  (RIP-relativo intra-funcion).  El encoder emite lea reg,[rip+disp32=0]
     *  + un @c MFixup{label} (mismo rel32 que jmp/jcc). */
    static MInstr make_lea_label(MOperand dst, uint32_t label_id) noexcept {
        MInstr i;
        i.op = MOp::LEA_LABEL;
        i.dst = dst;
        i.src1 = MOperand::make_label(label_id);
        return i;
    }

    /** @brief TLS_LE_ADDR: @p dst = direccion por-hilo del `thread_local`
     *  @p sym_idx (TLS local-exec, ELF).  El encoder emite
     *  `mov dst, %fs:0` + `lea dst, [dst + sym@tpoff]` con @c MReloc{TPOFF32}. */
    static MInstr make_tls_le_addr(MOperand dst, uint32_t sym_idx) noexcept {
        MInstr i;
        i.op = MOp::TLS_LE_ADDR;
        i.dst = dst;
        i.src1 = MOperand::make_imm32(static_cast<int32_t>(sym_idx));
        return i;
    }

    /** @brief TLS_PE_ADDR: @p dst = direccion por-hilo del `thread_local`
     *  @p var_sym_idx (.tls) usando el indice de slot @p index_sym_idx
     *  (`__vex_tls_index`).  El encoder emite la secuencia TEB del PE. */
    static MInstr make_tls_pe_addr(MOperand dst, uint32_t var_sym_idx,
                                   uint32_t index_sym_idx) noexcept {
        MInstr i;
        i.op = MOp::TLS_PE_ADDR;
        i.dst = dst;
        i.src1 = MOperand::make_imm32(static_cast<int32_t>(var_sym_idx));
        i.src2 = MOperand::make_imm32(static_cast<int32_t>(index_sym_idx));
        return i;
    }

    /** @brief DATA_PTR_LABEL: entrada de 8 bytes de la jump table densa que
     *  apunta al @p label_id (parchada post-memcpy). */
    static MInstr make_data_ptr_label(uint32_t label_id) noexcept {
        MInstr i;
        i.op = MOp::DATA_PTR_LABEL;
        i.src1 = MOperand::make_label(label_id);
        return i;
    }

    /** @brief DATA_REL32_LABEL: entrada self-relative de 4 bytes =
     *  offset[block_label] - offset[table_label] (jump table PIC-safe AOT). */
    static MInstr make_data_rel32_label(uint32_t block_label,
                                        uint32_t table_label) noexcept {
        MInstr i;
        i.op = MOp::DATA_REL32_LABEL;
        i.src1 = MOperand::make_label(block_label);
        i.src2 = MOperand::make_label(table_label);
        return i;
    }

    /** @brief JMP indirecto a registro (FF /4). */
    static MInstr make_jmp_reg(MReg r) noexcept {
        MInstr i;
        i.op = MOp::JMP;
        i.src1 = MOperand::make_reg(r, 8);
        return i;
    }
};

/* 40 bytes: 32 de codegen + 4 de @c ir_id (correlacion IR<->asm SOLO-LSP, vale
 * UINT32_MAX en produccion) + 4 de padding.  MachineIR es transitorio del
 * codegen (no es un path caliente de runtime como el bytecode), asi que el
 * coste es aceptable; el static_assert sigue blindando contra crecimiento
 * accidental mas alla de esto. */
static_assert(sizeof(MInstr) == 36,
              "MInstr debe ser 36 bytes (32 codegen + ir_id LSP)");

/* ===================================================================== */
/* MBlock                                                                 */
/* ===================================================================== */

using MBlockId = uint16_t;
using MLabelId = uint32_t;

static constexpr MBlockId MBLOCK_INVALID = static_cast<MBlockId>(-1);
static constexpr MLabelId MLABEL_INVALID = static_cast<MLabelId>(-1);

/**
 * @struct MBlock
 * @brief Bloque basico de MachineIR.
 *
 * Las instrucciones viven contiguas en @c instrs (vector).  Las
 * aristas del CFG se representan via @c succs (max 2 sucesores en
 * v1: caso normal o branch + fallthrough).  Predecesores se
 * calculan on-demand (no se mantienen para simplicidad).
 *
 * @c label_id es el label que apunta al inicio del bloque; permite
 * jumps internos a este bloque sin extra book-keeping.
 */
struct MBlock {
    MLabelId label_id = MLABEL_INVALID;
    std::vector<MInstr> instrs;
    MBlockId succ_a = MBLOCK_INVALID;
    MBlockId succ_b = MBLOCK_INVALID;
    /// Sucesores EXTRA / ABNORMALES (edges que no son el fallthrough ni el
    /// branch del terminador): handlers de excepcion (edge tryenter->catch)
    /// y, en el futuro, targets de jumptable/switch.  Vacio por defecto
    /// (cero overhead en el caso comun).  La liveness los UNE ademas de
    /// succ_a/succ_b para mantener vivos los valores live-in al sucesor a
    /// traves del edge anormal (p.ej. los valores que el catch usa).  La
    /// deteccion de loops y el encoder NO los consideran (no hay branch
    /// fisico hacia ellos; el control llega via runtime, p.ej. do_throw).
    std::vector<MBlockId> extra_succs;
    /// Offset en bytes desde el inicio del code cache donde se
    /// emite la primera instr de este bloque.  Lo poblea el encoder.
    uint32_t byte_offset = 0;
};

/* ===================================================================== */
/* MFunction                                                              */
/* ===================================================================== */

/**
 * @struct MFixup
 * @brief Pendiente de patch: una referencia a @c label_id en el
 *        codigo emitido que se debe rellenar tras conocerse el
 *        offset destino.
 *
 * Layout x86-64: las branches relativas usan @c rel32 al final de
 * la instruccion (justo despues del opcode + ModR/M).  El encoder
 * registra MFixup{label_id, offset_in_bytes, size=4, instr_end=
 * offset+4} y en una segunda pasada calcula el delta y patchea.
 */
struct MFixup {
    MLabelId label_id = MLABEL_INVALID;
    uint32_t patch_at = 0;  ///< byte offset donde escribir rel32
    uint32_t instr_end = 0; ///< byte offset del final del instr (base del rel)
    uint8_t width = 4;      ///< 1, 2, o 4 (siempre 4 en v1)
};

/* ===================================================================== */
/* Relocations AOT (resolucion CROSS-funcion / a datos)                   */
/* ===================================================================== */

/**
 * @enum MRelocKind
 * @brief Tipo de relocation que el codegen AOT deja sin resolver en una
 *        funcion compilada de forma aislada (Phase AOT.3 Paso 2b-ii).
 *
 * A diferencia de @c MFixup (intra-funcion: el encoder lo resuelve solo,
 * conoce el destino), una @c MReloc referencia un SIMBOLO cuya direccion
 * NO se conoce hasta el layout final de @c .text/.rodata (que hace el
 * driver tras colocar todas las funciones).  Es ARCH-AGNOSTICA: describe
 * "que parchear" (kind + offset + simbolo + addend), no como; cada
 * arquitectura interpreta @c kind a su modo (x86-64 rel32/abs64; ARM BL;
 * RISC-V JAL; ...).
 */
enum class MRelocKind : uint8_t {
    CALL_REL32 = 0, ///< x86-64 CALL/JMP E8/E9 rel32 a una FUNCION: *(int32*)@ =
                    ///< sym - (site+4).  El driver lo trata como callee (BFS).
    ABS64 = 1, ///< direccion absoluta 64-bit (mov reg,imm64): *(u64*)@ = sym +
               ///< addend. Ref a DATO (.rodata) en modo --no-pie.
    DATA_REL32 =
        2, ///< RIP-relativo a un DATO (lea reg,[rip+disp32]): *(int32*)@ =
           ///< sym - (site+4).  Misma matematica que CALL_REL32 pero el
           ///< target es un dato (.rodata), NO una funcion (el driver NO
           ///< lo encola como callee).  Default position-independent.
    TPOFF32 =
        3, ///< TLS local-exec (ELF): *(int32*)@ = offset del simbolo TLS
           ///< respecto al thread pointer (TP-relativo, NEGATIVO en la
           ///< variante II).  El driver lo traduce a R_X86_64_TPOFF32 (23)
           ///< sobre un simbolo STT_TLS; el `--link` resuelve el TPOFF.
    SECREL32 =
        4, ///< TLS PE (Windows): *(int32*)@ = offset del simbolo DENTRO de su
           ///< seccion (.tls), NO la VA.  El acceso suma este offset a la base
           ///< del bloque TLS del modulo (cargada desde el TEB).  El emisor PE
           ///< escribe target_off directamente.
};

/**
 * @struct MReloc
 * @brief Sitio del codigo de UNA funcion que referencia un simbolo externo
 *        a esa funcion (otra funcion del modulo, o un dato de @c .rodata) y
 *        que el driver parchea tras conocer el layout.
 *
 * @c patch_at es el offset en bytes DENTRO del codigo de la funcion (0 =
 * primer byte de la funcion); el driver le suma la base de la funcion en
 * @c .text para obtener el offset absoluto a parchear.  @c sym_idx indexa
 * @c MFunction::reloc_symbols.
 */
struct MReloc {
    MRelocKind kind = MRelocKind::CALL_REL32;
    uint32_t patch_at = 0; ///< byte offset dentro del codigo de la funcion
    uint32_t sym_idx = 0;  ///< indice en @c MFunction::reloc_symbols
    int64_t addend = 0;    ///< desplazamiento adicional dentro del simbolo
};

/* ===================================================================== */
/* LineMap (solo-LSP: vista "Godbolt" del codegen)                        */
/* ===================================================================== */

/**
 * @struct LineMapEntry
 * @brief Correlacion byte_offset (instruccion maquina) -> source_line (.vex).
 *
 * SOLO se poblea cuando @c MFunction::emit_line_map es true (modo de
 * analisis exclusivo del LSP).  En compilacion/ejecucion convencional el
 * flag esta OFF y el encoder NO construye la tabla -> cero overhead, bytes
 * de codigo BIT-IDENTICOS.  Una entrada por MInstr emitida, en orden de
 * emision (ascendente por @c byte_offset).  El inspector la cruza con el
 * desensamblado de Capstone para resaltar fuente <-> asm.
 */
struct LineMapEntry {
    uint32_t byte_offset = 0;  ///< Offset del primer byte de la instr (rel. fn).
    uint32_t source_line = 0;  ///< Linea .vex (1-based; 0 = sin atribucion).
    uint32_t ir_id = 0xFFFFFFFFu; ///< Identidad de la op IR origen (solo-LSP).
};

/* ===================================================================== */
/* Stackmaps (D.2-integration)                                            */
/* ===================================================================== */

/**
 * @enum StackmapGcKind
 * @brief Categoria del valor GC almacenado en un slot.
 */
enum class StackmapGcKind : uint8_t {
    HANDLE = 0,  ///< GcHandle (uint32_t en los 32 bits bajos del slot)
    HOSTPTR = 1, ///< host_ptr crudo apuntando al payload de un objeto GC
    STRING = 2   ///< StringObject* (host_ptr, scan especial)
};

/**
 * @struct StackmapSlot
 * @brief Descriptor de un slot stack que contiene un valor GC en un
 *        punto de safepoint determinado.
 *
 * Layout: 4 bytes empaquetados.
 *   +0 [2]  rbp_offset: offset signed desde RBP donde vive el slot
 *                       (tipicamente negativo: -8, -16, -24, ...).
 *   +2 [1]  gc_kind:    StackmapGcKind
 *   +3 [1]  _pad:       alineacion
 */
struct StackmapSlot {
    int16_t rbp_offset = 0;
    StackmapGcKind gc_kind = StackmapGcKind::HANDLE;
    uint8_t _pad = 0;
};

static_assert(sizeof(StackmapSlot) == 4, "StackmapSlot debe ser 4 bytes");

/**
 * @struct Stackmap
 * @brief Snapshot de slots GC vivos en un punto de safepoint.
 *
 * Cada @c MOp::SAFEPOINT y cada @c MOp::CALL genera un Stackmap que
 * el Selector poblea con los slots de SSA values @c is_gc_object=true
 * definidos hasta ese punto.  El @c pc_offset se rellena por el
 * encoder (tras saber donde quedo el poll en el code cache).
 *
 * El GC durante un sweep:
 *   1. Walk RBP chain.
 *   2. Por cada frame JIT, lookup en @c JitRegistry para encontrar
 *      su @c MFunction (o equivalente runtime).
 *   3. Binary search en @c stackmaps por @c pc_offset == frame.rip.
 *   4. Para cada slot del stackmap match: leer valor del slot
 *      (@c [rbp + rbp_offset]) y marcar segun @c gc_kind.
 */
struct Stackmap {
    uint32_t pc_offset = 0; ///< byte offset en code cache
    /// Tamano del frame en un safepoint call: RBP - RSP =
    /// pointer_size*callee_saved + spill_bytes.  Lo usa el WALK POR TAMANO DE
    /// FRAME del scan preciso de AOT (@c scan_aot_frames) para reconstruir RBP
    /// a partir del RSP del llamador SIN leer la cadena RBP -> robusto ante
    /// -fomit-frame-pointer / inlining de frames intermedios (modelo LLVM
    /// statepoint).  Es CONSTANTE por funcion; se replica en cada stackmap de
    /// la misma.  Queda 0 en el path JIT/interp (que camina la cadena RBP y no
    /// lo consume).
    uint32_t frame_size = 0;
    std::vector<StackmapSlot> slots;
};

/**
 * @struct AsmBlob
 * @brief Bloque de inline-asm nativo ya ensamblado (Phase AS inc.5).
 *
 * Lo referencia un @c MInstr de op @c INLINE_ASM_RAW via el indice en
 * @c MFunction::asm_blobs.  @c bytes es la salida de @c vex::g_asm_backend
 * (NASM Intel -> x86-64) que el encoder apendea verbatim.  El resto es
 * metadata que el regalloc consume para tratar el asm como un punto de
 * use/def + clobber sin descodificar los bytes:
 *
 *   - @c in_vregs / @c out_vregs : vregs (register-bound) leidos/escritos
 *     por el asm.  @c build_intervals los marca use/def en la posicion del
 *     INLINE_ASM_RAW para que sus intervalos cubran el asm y el regalloc no
 *     reuse sus registros fisicos (que ademas estan PINEADOS via
 *     @c MFunction::vreg_fixed).
 *   - @c clobbers : ids de registros fisicos (MReg) que el asm destruye y
 *     que NO son bindings.  El regalloc los excluye para los intervalos que
 *     cruzan esta posicion (igual semantica que un CALL caller-saved).
 *   - @c clobbers_flags / @c clobbers_mem : informativos (el JIT no modela
 *     un banco de flags ni reordena memoria a traves del asm en v1).
 */
struct AsmBlob {
    std::vector<uint8_t> bytes;      ///< x86-64 ya ensamblado
    std::vector<uint32_t> in_vregs;  ///< vregs leidos por el asm
    std::vector<uint32_t> out_vregs; ///< vregs escritos por el asm
    std::vector<uint8_t> clobbers;   ///< MReg ids clobbered (no bindings)
    bool clobbers_flags = false;
    bool clobbers_mem = false;
    /// Solo-inspeccion (LSP): etiquetas internas del asm -> offset RELATIVO al
    /// inicio del blob (bytes).  Las computa NUESTRO parser del texto usando el
    /// contrato insn_offsets del backend.  El encoder las reubica al offset
    /// absoluto de la funcion.  Vacio = sin info (degrada).
    std::vector<std::pair<uint32_t, std::string>> labels;
    /// Solo-inspeccion: offset relativo -> linea .vex de cada instruccion del
    /// asm (para atribuir cada instr a su linea real, no al `asm {` global).
    std::vector<std::pair<uint32_t, uint32_t>> insn_lines;
    /// Phase AS inc.6: simbolos PROPIOS referenciados desde el asm (`jmp
    /// [global]`, `mov rax, fn`, ...).  @c offset es RELATIVO al inicio del
    /// blob; el encoder lo reubica al offset de la funcion y emite un MReloc
    /// (DATA_REL32 si @c rip_relative, ABS64 si imm).  @c symbol es el nombre
    /// Vex (el driver lo resuelve a la dir de la funcion/dato).
    /// Tipo segun la forma de la instruccion (decidido por el usuario en el
    /// asm).  Espejo de @c vex::AsmAssembleResult::SymRefKind; el encoder lo
    /// mapea a @c MRelocKind.
    enum class AsmSymRefKind : uint8_t {
        BranchRel32, ///< jmp/call sym (directo) -> CALL_REL32
        DataRel32,   ///< jmp/call [sym] / lea [rip+sym] -> DATA_REL32
        Abs64,       ///< mov reg, sym -> ABS64
        Abs32,       ///< push sym / mov r32, sym -> ABS32 (best-effort)
    };
    struct AsmSymRef {
        uint32_t offset = 0;
        uint8_t size = 0;
        /// Bytes que siguen al campo disp DENTRO de la misma instruccion (p.ej.
        /// el imm32 de `mov [rip+disp32], imm32`).  Para DataRel32 el encoder
        /// ajusta el addend restando este valor (el disp rip-rel se mide desde
        /// el FIN de instruccion).
        uint8_t pcrel_trailing = 0;
        AsmSymRefKind kind = AsmSymRefKind::DataRel32;
        std::string symbol;
    };
    std::vector<AsmSymRef> sym_refs;
};

/**
 * @struct MFunction
 * @brief Funcion completa en MachineIR.
 *
 * Layout efficiente:
 *   - blocks: contiguos en memoria (vector).
 *   - imm64_pool: pool compartido de inmediatos 64-bit.
 *   - fixups: vector de patches pendientes tras la pasada de emit.
 *   - labels: mapping label_id -> byte_offset (poblado por encoder).
 */
struct MFunction {
    std::string name;
    std::vector<MBlock> blocks;
    std::vector<uint64_t> imm64_pool;
    std::vector<MFixup> fixups;
    /// Phase AOT.3 Paso 2b-ii: tabla de simbolos referenciados por las
    /// @c MReloc de esta funcion (nombres de funciones del modulo y de
    /// datos de .rodata).  Indexada por @c MReloc::sym_idx.
    std::vector<std::string> reloc_symbols;
    /// Phase AOT.3 Paso 2b-ii: relocations sin resolver que el encoder
    /// emite (CALL cross-funcion, refs a .rodata).  @c patch_at es relativo
    /// al inicio del codigo de ESTA funcion; el driver lo reubica al
    /// concatenar las funciones en @c .text.
    std::vector<MReloc> relocs;
    /// Offset de cada label en el code cache.  Indexado por label_id.
    /// Si el label no esta resuelto aun, contiene UINT32_MAX.
    std::vector<uint32_t> label_offsets;
    /// Jump table denso (SWITCH_DENSE): cada entrada DATA_PTR_LABEL emite 8
    /// bytes placeholder + registra aqui (patch_at, label del brazo).  Tras
    /// el memcpy al code cache (base conocida) el pipeline parchea:
    /// *(u64*)(base + patch_at) = base + label_offsets[label].  Es una
    /// direccion ABSOLUTA -> no se resuelve en resolve_fixups (que es rel32).
    struct AddrTableFixup {
        uint32_t patch_at;  ///< offset en el codigo de la entrada de 8 bytes
        MLabelId label;     ///< label del bloque destino (brazo o default)
    };
    std::vector<AddrTableFixup> addr_table_fixups;
    /// Tamano del frame stack (bytes) reservado por enter (sub rsp, N).
    /// Lo poblea el selector tras analizar locales/spills.
    uint32_t stack_frame_size = 0;
    /// Siguiente label_id disponible para new_label().
    uint32_t next_label_id = 0;
    /// D.2-int: stackmaps emitidos por el Selector en cada SAFEPOINT y
    /// CALL.  El encoder rellena @c pc_offset a medida que emite los
    /// bytes correspondientes (@c MOp::SAFEPOINT / @c MOp::CALL).
    ///
    /// Sorted post-encode por @c pc_offset para que el GC pueda hacer
    /// binary search durante stack walk.  Cada Stackmap describe los
    /// slots GC vivos en ese punto especifico.
    std::vector<Stackmap> stackmaps;

    /// Solo-LSP (vista "Godbolt"): si true, el encoder poblea @c line_map
    /// con una entrada por instruccion (byte_offset -> source_line).  Lo
    /// activa el Selector/vreg-select SOLO cuando el inspector del LSP pide
    /// el codegen anotado.  OFF por defecto -> el resto del proyecto
    /// (auto_jit, runtime, AOT normal) NO paga nada: el encoder ni mira la
    /// tabla y los bytes emitidos son identicos.
    bool emit_line_map = false;
    /// Phase NR: `@Naked` -- suprime prologo/epilogo Y ret implicito en el
    /// rewrite-to-physical.  El cuerpo (asm) provee su propia salida
    /// (ret/iretq).  Propagado desde @c IrFunction::is_naked por vreg-select.
    bool naked = false;
    /// Solo-LSP: correlacion byte_offset -> source_line.  Vacia salvo que
    /// @c emit_line_map este activo.  Ver @c LineMapEntry.
    std::vector<LineMapEntry> line_map;
    /// Solo-LSP: etiquetas internas de bloques inline-asm -> byte_offset
    /// (absoluto, relativo al inicio de la funcion).  El encoder las reubica
    /// desde @c AsmBlob::labels.  El inspector las muestra como divisores.
    std::vector<std::pair<uint32_t, std::string>> asm_labels;

    /// Sprint mem-loop-fix-v2 / fib-recursion (2026-06-02):
    /// indices del @c imm64_pool que contienen referencias a la
    /// PROPIA funcion (self-recursive call).  El encoder, al emitir
    /// un MOV reg,imm64 cuyo @c IMM64_IDX matchea, registra la
    /// posicion del imm64 en @c self_ref_byte_offsets.  Tras alocar
    /// el code cache, el JitCompiler escribe @c code_start en cada
    /// una de esas posiciones para resolver las self-refs.
    std::vector<uint32_t> self_ref_imm64_indices;
    /// Posiciones (byte offsets) DENTRO de la code emitida donde
    /// hay un imm64 self-ref que necesita patching.  Poblado por
    /// el encoder.
    std::vector<size_t> self_ref_byte_offsets;

    /// Phase D.7 (regalloc por vregs): numero de registros virtuales
    /// reservados en esta funcion.  Los ids son densos 0..vreg_count-1.
    /// Solo se usa en el path VREG (flag @c VESTA_JIT_VREGS); el path de
    /// slots lo deja en 0.
    uint32_t vreg_count = 0;
    /// OSR (Phase D.8): numero de valores IR originales (== fn.values.size()
    /// al compilar).  Los vregs [0, ir_value_count) corresponden 1:1 a IR
    /// value ids (mapeo identidad en @c vr()); los vregs >= ir_value_count
    /// son temporales internos del selector (intra-instruccion, nunca vivos
    /// a traves de un bloque).  El state-transfer del OSR (buffer-por-VID)
    /// SOLO captura vregs < ir_value_count: esos ids son ESTABLES entre C1 y
    /// C2 (el clon C2 preserva los IR VIDs), garantizando que C1 escribe y
    /// C2 lee la misma celda del buffer para el mismo valor logico.
    uint32_t ir_value_count = 0;
    /// Phase D.7: clase (GP/FP) de cada registro virtual, indexado por
    /// vreg id.  @c vreg_class.size() == @c vreg_count.  El register
    /// allocator la consulta para asignar del pool fisico correcto.
    std::vector<RegClass> vreg_class;
    /// Phase D.7 commit 5f: 1 si el vreg contiene un valor GESTIONADO por
    /// el GC (handle/host_ptr a objeto GC).  Lo poblea el selector desde
    /// @c IrValue::is_gc_object.  El pipeline lo usa para rechazar (sin
    /// stackmaps todavia) funciones donde un valor GC esta VIVO a traves
    /// de un call -> el GC no veria esa raiz si esta en un registro.
    /// @c vreg_is_gc.size() == @c vreg_count cuando esta poblado.
    std::vector<uint8_t> vreg_is_gc;
    /// Phase AS inc.5: registro fisico FORZADO (precoloreo) de un vreg, o
    /// -1 si libre.  SPARSE: no se mantiene paralelo a @c vreg_count; el
    /// selector solo lo redimensiona/poblea para los vregs register-bound de
    /// un inline-asm.  @c build_intervals lo copia a @c LiveInterval::fixed_reg
    /// (con bounds-check: @c vid < vreg_fixed.size() ? vreg_fixed[vid] : -1).
    std::vector<int8_t> vreg_fixed;
    /// Phase AS inc.5: bloques de inline-asm.  Indexados por el IMM32 de la
    /// MInstr @c INLINE_ASM_RAW (@c src1.value).
    std::vector<AsmBlob> asm_blobs;

    /// Phase AOT.3 Paso 2b: vreg ids de los parametros de la funcion, en
    /// orden de la convencion de llamada.  Solo lo usa el rewrite en ABI
    /// HOST_LEAF: los params llegan en los @c arg_regs del ABI host y se
    /// copian a su ubicacion fisica con un parallel-move en el prologo (en
    /// VM_ABI el selector ya los carga desde @c proc->registers, por lo que
    /// este campo se ignora).  @c param_vregs[i] == @c IrFunction::params[i].
    std::vector<uint32_t> param_vregs;

    /** @brief Marca el vreg @p vid como precoloreado al fisico @p phys
     *  (Phase AS inc.5).  Redimensiona @c vreg_fixed perezosamente. */
    void set_vreg_fixed(uint32_t vid, uint8_t phys) {
        if (vreg_fixed.size() <= vid) vreg_fixed.resize(vid + 1, -1);
        vreg_fixed[vid] = static_cast<int8_t>(phys);
    }
    /** @brief Registro fisico forzado de @p vid, o -1 si libre. */
    int fixed_of(uint32_t vid) const noexcept {
        return vid < vreg_fixed.size() ? vreg_fixed[vid] : -1;
    }
    /** @brief añade un @c AsmBlob al pool y devuelve su indice. */
    uint32_t intern_asm_blob(AsmBlob b) {
        asm_blobs.push_back(std::move(b));
        return static_cast<uint32_t>(asm_blobs.size() - 1);
    }

    /** @brief Reserva un nuevo label_id (zero overhead). */
    MLabelId new_label() {
        const uint32_t id = next_label_id++;
        if (label_offsets.size() <= id) {
            label_offsets.resize(id + 1, UINT32_MAX);
        }
        return id;
    }

    /** @brief Crea un nuevo bloque vacio.  Devuelve su id. */
    MBlockId new_block(MLabelId lbl) {
        MBlock b;
        b.label_id = lbl;
        blocks.push_back(std::move(b));
        return static_cast<MBlockId>(blocks.size() - 1);
    }

    /** @brief Internaliza el nombre de simbolo @p name en
     *  @c reloc_symbols (deduplicado) y devuelve su indice (AOT). */
    uint32_t intern_reloc_symbol(const std::string &name) {
        for (uint32_t i = 0; i < reloc_symbols.size(); ++i)
            if (reloc_symbols[i] == name) return i;
        reloc_symbols.push_back(name);
        return static_cast<uint32_t>(reloc_symbols.size() - 1);
    }

    /** @brief añade un imm64 al pool y devuelve su indice. */
    uint32_t intern_imm64(uint64_t value) {
        /* O(n) lookup para deduplicar.  Aceptable para el v1: el
         * codigo Vex tipico tiene < 100 imm64 distintos por funcion. */
        for (uint32_t i = 0; i < imm64_pool.size(); ++i) {
            if (imm64_pool[i] == value) return i;
        }
        imm64_pool.push_back(value);
        return static_cast<uint32_t>(imm64_pool.size() - 1);
    }

    /**
     * @brief Reserva un nuevo registro virtual de la clase dada (D.7).
     *
     * Devuelve el operando @c VREG listo para usar como dst/src de una
     * MInstr.  Los temporales que introduce el selector (no derivados
     * directamente de un SSA value) se piden por aqui.
     *
     * @param cls    Clase del registro (GP/FP).
     * @param width  Ancho en bytes (1/2/4/8).  Default 8.
     * @return       Operando VREG con el id recien reservado.
     */
    MOperand new_vreg(RegClass cls, uint8_t width = 8) {
        const uint32_t id = vreg_count++;
        vreg_class.push_back(cls);
        return MOperand::make_vreg(id, cls, width);
    }
};

} // namespace jit

#endif // VESTA_JIT_MACHINE_IR_H
