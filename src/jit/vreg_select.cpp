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
#include "jit/target_reginfo.h" // Phase AOT.3 2b: arg_regs del ABI host (HOST_LEAF)
#include "gc/raw_allocator.h" // Phase D.7 perf: inline slab fast-path
#include "vex/asm_backend.h"  // Phase AS inc.5: ensamblar inline-asm -> bytes
#include "vex/asm_effects.h"  // Phase AS inc.5: asm_canonical_reg
/* arena -> windows.h (Win32) define macros que chocan con nombres del enum
 * IrOp/IrType (CONST, VOID, etc.).  Deshacerlos para no romper ir::IrOp::CONST.
 */
#ifdef CONST
#undef CONST
#endif
#ifdef VOID
#undef VOID
#endif

#include <cstdio>
#include <cstdlib>
#include <utility> // std::swap (FCMP operand reorder)
#include <vector>  // critical-edge splitting (pred_count, working copy)

namespace jit {

/** @brief Diagnostico opt-in (VESTA_JIT_VREGS_DEBUG=1) de por que una
 *  funcion no es seleccionable por el path vreg. */
static void vreg_dbg(const char *fn, const char *op) {
    static const bool on = [] {
        const char *v = std::getenv("VESTA_JIT_VREGS_DEBUG");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    if (on)
        std::fprintf(stderr, "[vreg-sel] '%s' no soportada: op %s\n", fn, op);
}

/** @brief Diagnostico/A-B: VESTA_JIT_NO_INLINE_DEREF=1 enruta GC_DEREF_HOST
 *  al CALL @c vrt_gc_deref en vez del inline (para medir el inline vs el
 *  runtime, o aislar un posible bug del codegen inline). */
static bool jit_no_inline_deref() {
    static const bool off = [] {
        const char *v = std::getenv("VESTA_JIT_NO_INLINE_DEREF");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return off;
}

/** @brief Diagnostico/A-B: VESTA_JIT_NO_INLINE_ALLOC=1 enruta RAW_ALLOC al
 *  CALL @c vrt_raw_alloc en vez de inline-ar el fast-path del slab (para
 *  medir el inline vs el runtime, o aislar un bug del codegen inline). */
static bool jit_no_inline_alloc() {
    static const bool off = [] {
        const char *v = std::getenv("VESTA_JIT_NO_INLINE_ALLOC");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return off;
}

/** @brief DIV/MOD enteros en el path de vregs via el pseudo DIVMOD_V
 *  (expandido a idiv en el rewrite usando R11 scratch para el divisor).
 *  Default ON tras validar (codegen verificado en test_vreg_vm + e2e
 *  400/2 bajo JIT; el bug de IMUL+spill que lo bloqueaba se arreglo en
 *  regalloc_rewrite).  Se puede DESACTIVAR con VESTA_JIT_VREG_IDIV=0
 *  para volver al fallback de slots (A/B). */
static bool jit_vreg_idiv() {
    static const bool on = [] {
        const char *v = std::getenv("VESTA_JIT_VREG_IDIV");
        return !(v && v[0] == '0'); // default ON; solo OFF si =0
    }();
    return on;
}

/** @brief Inline dispatch de CALLVIRT en el path de vregs (default ON;
 *  VESTA_JIT_NO_INLINE_CALLVIRT=1 vuelve al CALL a vrt_callvirt para A/B).
 *  Carga class_ptr -> vtable -> method -> jit_code y hace un call directo
 *  (indirecto) si el metodo esta JIT-compilado y sin advices, evitando el
 *  overhead del helper runtime; fallback a vrt_callvirt en cualquier otro
 *  caso (null, abstracto, no compilado, con aspectos AOP). */
static bool jit_no_inline_callvirt() {
    static const bool off = [] {
        const char *v = std::getenv("VESTA_JIT_NO_INLINE_CALLVIRT");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return off;
}

namespace {

/** @brief Operando VREG (clase GP) para un IrValueId. */
inline MOperand vr(ir::IrValueId v) {
    return MOperand::make_vreg(static_cast<uint32_t>(v), RegClass::GP, 8);
}

/** @brief Tamano en bytes de un IrType entero/puntero. */
inline int ir_type_bytes(ir::IrType t) {
    switch (t) {
    case ir::IrType::I8:
    case ir::IrType::U8:
    case ir::IrType::BOOL: return 1;
    case ir::IrType::I16:
    case ir::IrType::U16: return 2;
    case ir::IrType::I32:
    case ir::IrType::U32:
    case ir::IrType::F32: return 4;
    default: return 8; // I64/U64/F64/PTR/HANDLE
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
    return MOperand::make_mem(MReg::RBX, VESTA_PROC_REGISTERS_OFFSET +
                                             j * VESTA_REGISTER_SIZE);
}

/** @brief Mapea un IrOp de ALU binaria al MOp x86, o false si no aplica. */
inline bool bin_mop(ir::IrOp op, MOp &out) {
    switch (op) {
    case ir::IrOp::ADD: out = MOp::ADD; return true;
    case ir::IrOp::SUB: out = MOp::SUB; return true;
    case ir::IrOp::MUL: out = MOp::IMUL; return true;
    case ir::IrOp::AND: out = MOp::AND; return true;
    case ir::IrOp::OR: out = MOp::OR; return true;
    case ir::IrOp::XOR: out = MOp::XOR; return true;
    default: return false;
    }
}

/** @brief Condicion x86 para un CMP_* del IR, o false si no es CMP. */
inline bool cmp_cond(ir::IrOp op, MCond &cc) {
    switch (op) {
    case ir::IrOp::CMP_EQ: cc = MCond::E; return true;
    case ir::IrOp::CMP_NE: cc = MCond::NE; return true;
    case ir::IrOp::CMP_LT: cc = MCond::L; return true; // signed
    case ir::IrOp::CMP_GT: cc = MCond::G; return true;
    case ir::IrOp::CMP_LE: cc = MCond::LE; return true;
    case ir::IrOp::CMP_GE: cc = MCond::GE; return true;
    case ir::IrOp::CMP_ULT: cc = MCond::B; return true; // unsigned
    case ir::IrOp::CMP_UGT: cc = MCond::A; return true;
    case ir::IrOp::CMP_ULE: cc = MCond::BE; return true;
    case ir::IrOp::CMP_UGE: cc = MCond::AE; return true;
    default: return false;
    }
}

/** @brief MInstr CMP a, b (forma 3-op: src1=a, src2=b; el rewrite la baja). */
inline MInstr mk_cmp(ir::IrValueId a, ir::IrValueId b) {
    MInstr i;
    i.op = MOp::CMP;
    i.src1 = vr(a);
    i.src2 = vr(b);
    return i;
}
/** @brief MInstr TEST a, b. */
inline MInstr mk_test(ir::IrValueId a, ir::IrValueId b) {
    MInstr i;
    i.op = MOp::TEST;
    i.src1 = vr(a);
    i.src2 = vr(b);
    return i;
}
/** @brief MInstr SETcc dst (variant=cc). */
inline MInstr mk_setcc(ir::IrValueId dst, MCond cc) {
    MInstr i;
    i.op = MOp::SETCC;
    i.dst = vr(dst);
    i.variant = static_cast<uint8_t>(cc);
    return i;
}

/** @brief Cuenta cuantas veces se usa el SSA value @p v en toda la funcion
 *  (operandos + func_ptr + phi_args).  Se usa para decidir si un CMP puede
 *  fusionarse con su BR_COND: la fusion (CMP + Jcc) NO materializa el bool en
 *  un registro, asi que solo es segura si el resultado del CMP se usa
 *  EXCLUSIVAMENTE en ese BR_COND (uses == 1).  Con >1 uso (p.ej. el mismo bool
 *  alimenta un segundo BR_COND en otro bloque) hay que materializarlo via
 *  SETcc; si no, el segundo uso leeria un registro sin escribir. */
inline size_t vreg_count_uses(const ir::IrFunction &fn, ir::IrValueId v) {
    if (v == ir::IR_NO_VALUE) return 0;
    size_t n = 0;
    for (const auto &b : fn.blocks)
        for (const auto &in : b.instrs) {
            for (ir::IrValueId op : in.operands)
                if (op == v) n = n + 1;
            if (in.func_ptr == v) n = n + 1;
            for (const auto &pa : in.phi_args)
                if (pa.value == v) n = n + 1;
        }
    return n;
}

/** @brief True si el bloque @p b tiene alguna instr PHI. */
inline bool block_has_phi(const ir::IrBlock &b) {
    for (const auto &in : b.instrs)
        if (in.op == ir::IrOp::PHI) return true;
    return false;
}

/**
 * @brief CRITICAL-EDGE SPLITTING (pre-pase out-of-SSA).
 *
 * Inserta un bloque puente vacio en cada arista pred->succ donde @c pred
 * tiene >1 sucesor (BR_COND) y @c succ tiene PHIs.  El problema: las copias
 * de PHI (phi_dst <- pred_value) se colocan al FINAL del predecesor, lo que
 * las ejecutaria TAMBIEN cuando @c pred salta a su OTRO sucesor.  El puente
 * rompe esto: pred->puente->succ, con el puente teniendo 1 solo sucesor, asi
 * la copia (emitida al final del puente) corre exclusivamente en esa arista.
 *
 * Nota: esto cubre tanto las aristas CRITICAS estrictas (succ con >1 pred)
 * como el caso pred-multi-succ / succ-1-pred (donde la copia al final del
 * pred tambien seria incorrecta porque pred tiene otra rama).  Un puente de
 * sobra es siempre correcto (un JMP extra, sin coste observable).
 *
 * CRITICO -- ORDEN DE LAYOUT: el constructor de live-intervals
 * (@c build_intervals) asigna POSICIONES LINEALES por orden de
 * almacenamiento de @c fn.blocks; un valor definido en @c pred y consumido
 * por la copia del puente debe tener su rango SIN un hueco que el
 * linear-scan reutilice (si el puente quedara al final, las posiciones de
 * @c then/@c merge caerian ENTRE el def y el uso del puente, y el regalloc
 * reasignaria el registro del valor -> lost-copy).  Por eso cada puente se
 * inserta INMEDIATAMENTE DESPUES de su predecesor en el layout (orden de
 * ejecucion valido); luego se RENUMERAN todos los block-ids (target_block,
 * false_block, phi_args.block, preds, succs, id) via un remap old->new.
 *
 * @return true si modifico la funcion (split aplicado).
 */
inline bool split_critical_edges(ir::IrFunction &fn) {
    const size_t NB0 = fn.blocks.size();

    /* Fase 1: detectar las aristas a partir y crear los puentes con IDs
     * TEMPORALES (>= NB0).  Redirigir terminadores + PHI args + preds/succs
     * en el espacio de IDs original + temporal.  @c bridge_after[pred]
     * lista los puentes que deben ir tras ese predecesor en el layout. */
    std::vector<ir::IrBlock> bridges;
    std::vector<std::vector<ir::IrBlockId>> bridge_after(NB0);
    auto next_id = [&]() {
        return static_cast<ir::IrBlockId>(NB0 + bridges.size());
    };

    for (size_t b = 0; b < NB0; ++b) {
        /* Localizar el BR_COND terminador (unico con 2 sucesores). */
        ir::IrInstr *term = nullptr;
        for (auto it = fn.blocks[b].instrs.rbegin();
             it != fn.blocks[b].instrs.rend(); ++it) {
            if (it->op == ir::IrOp::BR_COND) { term = &*it; break; }
            if (it->op == ir::IrOp::BR || it->op == ir::IrOp::RET ||
                it->op == ir::IrOp::THROW || it->op == ir::IrOp::TAILCALL)
                break;
        }
        if (term == nullptr) continue;
        if (term->target_block == term->false_block) continue; // 1 succ real

        for (int side = 0; side < 2; ++side) {
            const ir::IrBlockId succ =
                (side == 0) ? term->target_block : term->false_block;
            if (succ >= NB0) continue;
            if (!block_has_phi(fn.blocks[succ])) continue;

            const ir::IrBlockId bridge = next_id();
            ir::IrBlock bb;
            bb.id = bridge;
            bb.name = "crit_edge_split";
            ir::IrInstr br;
            br.op = ir::IrOp::BR;
            br.target_block = succ;
            bb.instrs.push_back(br);
            bb.preds.push_back(static_cast<ir::IrBlockId>(b));
            bb.succs.push_back(succ);

            if (side == 0)
                term->target_block = bridge;
            else
                term->false_block = bridge;

            /* Los PHI args del succ que venian de @c b ahora vienen del
             * puente. */
            for (ir::IrInstr &p : fn.blocks[succ].instrs) {
                if (p.op != ir::IrOp::PHI) continue;
                for (ir::IrPhiArg &a : p.phi_args)
                    if (a.block == static_cast<ir::IrBlockId>(b))
                        a.block = bridge;
            }
            for (ir::IrBlockId &s : fn.blocks[b].succs)
                if (s == succ) s = bridge;
            for (ir::IrBlockId &pr : fn.blocks[succ].preds)
                if (pr == static_cast<ir::IrBlockId>(b)) pr = bridge;

            bridge_after[b].push_back(bridge);
            bridges.push_back(std::move(bb));
        }
    }
    if (bridges.empty()) return false;

    /* Fase 2: construir el nuevo layout (cada puente justo tras su pred) y
     * el remap old_id -> new_id.  @c temp_id de un puente = NB0 + indice en
     * @c bridges. */
    std::vector<ir::IrBlock> laid;
    laid.reserve(NB0 + bridges.size());
    std::vector<ir::IrBlockId> remap(NB0 + bridges.size(), ir::IR_NO_BLOCK);
    for (size_t b = 0; b < NB0; ++b) {
        remap[b] = static_cast<ir::IrBlockId>(laid.size());
        laid.push_back(std::move(fn.blocks[b]));
        for (ir::IrBlockId tmp : bridge_after[b]) {
            const size_t bidx = static_cast<size_t>(tmp) - NB0;
            remap[tmp] = static_cast<ir::IrBlockId>(laid.size());
            laid.push_back(std::move(bridges[bidx]));
        }
    }

    /* Fase 3: aplicar el remap a todas las referencias de block-id. */
    auto rm = [&](ir::IrBlockId id) -> ir::IrBlockId {
        return (id < remap.size() && remap[id] != ir::IR_NO_BLOCK) ? remap[id]
                                                                   : id;
    };
    for (ir::IrBlock &blk : laid) {
        blk.id = rm(blk.id);
        for (ir::IrBlockId &p : blk.preds) p = rm(p);
        for (ir::IrBlockId &s : blk.succs) s = rm(s);
        for (ir::IrInstr &in : blk.instrs) {
            if (in.target_block != ir::IR_NO_BLOCK)
                in.target_block = rm(in.target_block);
            if (in.false_block != ir::IR_NO_BLOCK)
                in.false_block = rm(in.false_block);
            for (ir::IrPhiArg &a : in.phi_args) a.block = rm(a.block);
        }
    }
    fn.blocks = std::move(laid);
    return true;
}

/** @brief True si la funcion tiene >=1 arista (pred BR_COND de 2 vias) hacia
 *  un bloque con PHIs (solo entonces hace falta copiar la funcion + split). */
inline bool has_critical_edge_to_phi(const ir::IrFunction &fn) {
    const size_t NB0 = fn.blocks.size();
    for (size_t b = 0; b < NB0; ++b) {
        ir::IrBlockId tt = ir::IR_NO_BLOCK, tf = ir::IR_NO_BLOCK;
        for (auto it = fn.blocks[b].instrs.rbegin();
             it != fn.blocks[b].instrs.rend(); ++it) {
            if (it->op == ir::IrOp::BR_COND) {
                tt = it->target_block;
                tf = it->false_block;
                break;
            }
            if (it->op == ir::IrOp::BR || it->op == ir::IrOp::RET ||
                it->op == ir::IrOp::THROW || it->op == ir::IrOp::TAILCALL)
                break;
        }
        if (tt == ir::IR_NO_BLOCK || tt == tf) continue; // no 2-via
        for (ir::IrBlockId succ : {tt, tf}) {
            if (succ >= NB0) continue;
            if (block_has_phi(fn.blocks[succ])) return true;
        }
    }
    return false;
}

/**
 * @brief Phase AS inc.5: mapea un nombre de registro CANONICO de 64
 *        bits (de @c vex::asm_canonical_reg) al id de @c MReg GP (0..15),
 *        o -1 si no es un GP usable como pin de inline-asm.
 *
 * RECHAZA explicitamente rsp/rbp (frame del JIT) y rbx (ProcessVM* en
 * VM_ABI); pinear un binding a esos corromperia el frame/contexto.  Los
 * registros vectoriales ("vN") devuelven -1 (el banco FP no es asignable
 * en el regalloc v1).  El selector cae a fallback si recibe -1.
 */
inline int canon_gp_to_mreg(const std::string &c) {
    static const struct {
        const char *n;
        int r;
    } T[] = {
        {"rax", 0},  {"rcx", 1},  {"rdx", 2},  {"rsi", 6},  {"rdi", 7},
        {"r8", 8},   {"r9", 9},   {"r10", 10}, {"r11", 11}, {"r12", 12},
        {"r13", 13}, {"r14", 14}, {"r15", 15},
        /* rbx(3)/rsp(4)/rbp(5) RESERVADOS: no se exponen como pin. */
    };
    for (const auto &e : T)
        if (c == e.n) return e.r;
    return -1;
}

} // namespace

bool vreg_select(const ir::IrFunction &fn_in, MFunction &out, AbiKind abi,
                 const CallResolver &resolve_call, const VregEntries &ent,
                 const CallResolver &resolve_native,
                 const CallResolver &resolve_symbol, bool pic, bool target_sysv,
                 bool mode32, FloatIsa fisa, bool emit_line_map) {
    /* CRITICAL-EDGE SPLITTING (out-of-SSA): si hay >=1 arista critica que
     * entra a un bloque con PHIs, trabajamos sobre una COPIA de la funcion
     * con los puentes insertados (zero-cost para el caso comun sin aristas
     * criticas, que ni copia ni recorre de nuevo).  El resto del selector
     * usa @c fn (que apunta a la copia o al original). */
    ir::IrFunction fn_storage;
    const ir::IrFunction *fn_ptr = &fn_in;
    static const bool no_split = [] {
        const char *v = std::getenv("VESTA_VREG_NO_SPLIT");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    /* El split se aplica en el path AOT (HOST_LEAF), donde es el bloqueante
     * real (un `if(c){n=v}` con PHI en el merge no compilaba) y el codegen es
     * el ABI nativo del target sin traduccion vm_mem -> seguro (validado por
     * ejecucion).  En VM_ABI (JIT en proceso) se MANTIENE el comportamiento
     * previo (rechazo de la arista critica -> fallback al selector de slots):
     * habilitar vreg en VM_ABI para estas funciones destapa un bug LATENTE,
     * INDEPENDIENTE del split, del paso de punteros a ALLOCA de la VM-stack
     * entre funciones vreg (reproducible sin el split); ese fix es de otro
     * sprint.  Asi el JIT no regresiona (las funciones que antes caian a
     * slots siguen cayendo) y el AOT gana las aristas criticas. */
    if (!no_split && abi == AbiKind::HOST_LEAF &&
        has_critical_edge_to_phi(fn_in)) {
        fn_storage = fn_in;
        split_critical_edges(fn_storage);
        fn_ptr = &fn_storage;
    }
    const ir::IrFunction &fn = *fn_ptr;

    /* HOST_LEAF (AOT): arg_regs del ABI del TARGET (SysV para ELF, Win64 para
     * PE), NO del host -> permite cross-target (ELF en Windows, PE en Linux).
     * x86-32 (mode32): regparm(3) (EAX/EDX/ECX).  En VM_ABI (JIT en proceso)
     * no se usa (el codigo VM usa el ABI host). */
    const TargetRegInfo &tri_sel =
        mode32 ? target_x86_32() : target_x86_64_abi(target_sysv);
    const size_t host_leaf_nmax =
        tri_sel.arg_regs[static_cast<size_t>(RegClass::GP)].size();
    out = MFunction{};
    out.name = fn.name;
    /* Phase NR @Naked: propagar para suprimir prologo/epilogo/ret en el
     * rewrite-to-physical.  El cuerpo (asm) provee su propia salida. */
    out.naked = fn.is_naked;
    /* Solo-LSP (vista "Godbolt"): activar la captura de source_line en el
     * codegen vreg (AOT).  OFF por defecto -> sin efecto en produccion. */
    out.emit_line_map = emit_line_map;
    out.vreg_count = static_cast<uint32_t>(fn.values.size());
    out.ir_value_count =
        static_cast<uint32_t>(fn.values.size()); // OSR: limite IR/temps
    out.vreg_class.assign(fn.values.size(), RegClass::GP);
    const bool vm = (abi == AbiKind::VM);

    /* FP-regalloc (Phase AOT C1 float): el codegen float SSE2 (FP residente
     * en XMM) solo se activa en HOST_LEAF + SSE2 + 64-bit.  En cualquier otro
     * caso (VM_ABI del JIT en proceso, x86-32, o FloatIsa != SSE2) las ops
     * float caen al fallback (false) -> el path de slots / el interp se
     * encargan (sin regresion).  Los backends x87/AVX/AVX512F/AUTO son slices
     * futuros. */
    const bool fp_ok = (abi == AbiKind::HOST_LEAF) &&
                       (fisa == FloatIsa::SSE2) && !mode32;
    /* Marcar la clase FP de cada vreg float (F32/F64).  El linear_scan por
     * clase les asigna XMM; el rewrite materializa con MOVSD/MOVSS.  Solo si
     * fp_ok (en otro caso ningun op float llega a emitirse). */
    if (fp_ok)
        for (size_t i = 0; i < fn.values.size(); ++i)
            if (ir_type_is_float(fn.values[i].type))
                out.vreg_class[i] = RegClass::FP;

    /* Phase D.7 commit 5f: marcar los vregs GC.  El pipeline hace el
     * check FINO (sin stackmaps todavia): rechaza la funcion solo si un
     * valor GC esta VIVO a traves de un call (su intervalo cubre un
     * call_position) -- ese seria invisible al GC en un registro.  Los
     * receptores/args de un call van a proc->registers (que el GC SI
     * escanea) y mueren antes del call, asi que no disparan el rechazo. */
    /* Phase AOT.3 2b: exponer los vregs de los parametros al rewrite (los
     * usa en HOST_LEAF para el parallel-move de los arg_regs en el prologo;
     * en VM_ABI se ignora -- el selector carga los params desde memoria). */
    out.param_vregs.assign(fn.params.begin(), fn.params.end());

    out.vreg_is_gc.assign(fn.values.size(), 0);
    for (size_t i = 0; i < fn.values.size(); ++i) {
        if (!fn.values[i].is_gc_object) continue;
        /* Codifica StackmapGcKind+1: HOSTPTR si es host_ptr a objeto GC,
         * HANDLE en otro caso.  (STRING se trata como HOSTPTR: el scan
         * usa el mismo host_ptr al payload.) */
        const StackmapGcKind k = fn.values[i].is_host_ptr
                                     ? StackmapGcKind::HOSTPTR
                                     : StackmapGcKind::HANDLE;
        out.vreg_is_gc[i] = static_cast<uint8_t>(static_cast<uint8_t>(k) + 1u);
    }

    /* String ops que devuelven un GcHandle (STRMAKE/STRCAT).  El IR NO los
     * marca @c is_gc_object (el handle es indice estable que no se mueve;
     * marcarlo romperia el save_live_regs del interp, que aplicaria
     * gchandle sobre un valor que YA es handle -- ver lowering emit_strmake).
     * PERO la StringObject referenciada DEBE sobrevivir un GC si el handle
     * vive a traves de otra call que aloque (otra STRMAKE/STRCAT/NEWOBJ).
     * Los strings pequenos se pinean via gc_addref (intern), pero los
     * grandes NO -> marcar el dst como root de tipo HANDLE para que el
     * regalloc lo spillee + stackmapee (commit 6) cuando cruza un call.
     * Coste cero si no cruza ninguno (no hay spill). */
    for (const auto &blk : fn.blocks)
        for (const auto &ins2 : blk.instrs)
            if ((ins2.op == ir::IrOp::STRMAKE || ins2.op == ir::IrOp::STRCAT) &&
                ins2.dst != ir::IR_NO_VALUE &&
                ins2.dst < out.vreg_is_gc.size() &&
                out.vreg_is_gc[ins2.dst] == 0)
                out.vreg_is_gc[ins2.dst] =
                    static_cast<uint8_t>(StackmapGcKind::HANDLE) + 1u;

    const size_t NB = fn.blocks.size();
    if (NB == 0) return false;

    /* Un label por bloque (MBlock index == IR block id). */
    std::vector<MLabelId> blbl(NB);
    for (size_t b = 0; b < NB; ++b)
        blbl[b] = out.new_label();

    /* Valores const conocidos (para shifts por cantidad inmediata).  El
     * IR define la cantidad de un shift con un CONST previo; lo capturamos
     * para emitir SHL/SHR/SAR dst, imm en vez de via CL. */
    std::vector<uint8_t> v_is_const(fn.values.size(), 0);
    std::vector<int64_t> v_const(fn.values.size(), 0);

    /* Vids que son ALLOCA host-stack (viven en el frame, liberados por el
     * epilogue).  Un RAW_FREE sobre uno de estos es NO-OP (no se llama a
     * vrt_raw_free, que crashearia sobre un ptr de host stack). */
    std::vector<uint8_t> v_is_host_alloca(fn.values.size(), 0);
    for (const auto &blk : fn.blocks)
        for (const auto &ins2 : blk.instrs)
            if (ins2.op == ir::IrOp::ALLOCA && ins2.host_alloca &&
                ins2.dst != ir::IR_NO_VALUE &&
                ins2.dst < v_is_host_alloca.size())
                v_is_host_alloca[ins2.dst] = 1u;

    /* ---- Phase AS inc.5: inline-asm (register-bound vars) ----
     * Una funcion con @c asm_reg_bindings tiene >=1 bloque INLINE_ASM.  Las
     * vars @c register("reg") viven en un ALLOCA estable (inc.3); aqui las
     * COLAPSAMOS a un vreg PRECOLOREADO a su registro fisico:
     *   - el ALLOCA del binding NO emite host-slot (se salta);
     *   - un STORE a su alloca  -> MOV vbind <- val  (carga del input);
     *   - un LOAD de su alloca  -> MOV dst <- vbind  (lectura del output);
     *   - el INLINE_ASM se ensambla a bytes (g_asm_backend) y se emite como
     *     INLINE_ASM_RAW; sus in/out vregs (clasificados aqui) marcan la
     *     liveness para que el regalloc respete el pin.
     * @c binding_phys[vid] = reg fisico (0..15) o -1.  Clasificacion in/out
     * por uso real: input si hay STORE a su alloca, output si hay LOAD. */
    std::vector<int> binding_phys(fn.values.size(), -1);
    std::vector<uint8_t> binding_is_in(fn.values.size(), 0);
    std::vector<uint8_t> binding_is_out(fn.values.size(), 0);
    const bool has_inline_asm = !fn.asm_reg_bindings.empty();
    if (has_inline_asm) {
        /* El ensamblado requiere un backend activo (lo registra main.cpp). */
        if (vex::g_asm_backend == nullptr) {
            vreg_dbg(fn.name.c_str(), "inline-asm(no-backend)");
            return false;
        }
        for (const ir::AsmRegBinding &b : fn.asm_reg_bindings) {
            if (b.alloca_value >= fn.values.size()) {
                vreg_dbg(fn.name.c_str(), "inline-asm(binding-oob)");
                return false;
            }
            if (b.is_vector) { /* banco FP no asignable en regalloc v1 */
                vreg_dbg(fn.name.c_str(), "inline-asm(vector-bind)");
                return false;
            }
            const std::string canon = vex::asm_canonical_reg(b.reg);
            const int phys = canon_gp_to_mreg(canon);
            if (phys < 0) { /* reservado (rbx/rsp/rbp) o no GP -> fallback */
                vreg_dbg(fn.name.c_str(), "inline-asm(reg-no-usable)");
                return false;
            }
            binding_phys[b.alloca_value] = phys;
            out.set_vreg_fixed(static_cast<uint32_t>(b.alloca_value),
                               static_cast<uint8_t>(phys));
        }
        /* Clasificar in/out por STORE/LOAD a cada alloca de binding. */
        for (const auto &blk : fn.blocks)
            for (const auto &ins2 : blk.instrs) {
                if (ins2.op == ir::IrOp::STORE && ins2.operands.size() == 2 &&
                    ins2.operands[1] < binding_phys.size() &&
                    binding_phys[ins2.operands[1]] >= 0)
                    binding_is_in[ins2.operands[1]] = 1u;
                if (ins2.op == ir::IrOp::LOAD && ins2.operands.size() == 1 &&
                    ins2.operands[0] < binding_phys.size() &&
                    binding_phys[ins2.operands[0]] >= 0)
                    binding_is_out[ins2.operands[0]] = 1u;
            }
    }

    /* Crea un vreg temporal nuevo (GP, no-GC) para secuencias inline
     * (p.ej. el inline de vmath_abs). */
    auto new_tmp = [&]() -> ir::IrValueId {
        const ir::IrValueId id = out.vreg_count++;
        out.vreg_class.push_back(RegClass::GP);
        out.vreg_is_gc.push_back(0);
        return static_cast<ir::IrValueId>(id);
    };

    /* Devirtualizacion de punteros de funcion conocidos: si un value viene de
     * un LABEL_ADDR (direccion de una funcion concreta), guardamos su nombre.
     * CALLCLOSURE/CALLIND cuyo func_ptr este aqui se bajan a un CALL DIRECTO
     * (CALL_SYM) en vez de una llamada indirecta -> sin indireccion (mejor
     * que un puntero de funcion C).  Caso tipico: lambda sin capturas llamada
     * en el mismo scope donde se define. */
    std::unordered_map<ir::IrValueId, std::string> label_fn;

    /* FP-regalloc: operando VREG de un value IR con su CLASE (FP si el value
     * es float y fp_ok) y ANCHO correctos (4 para f32, 8 para f64/resto).
     * El rewrite consulta la clase (@c is_fp_operand) para enrutar los MOV
     * float al MOVSD/MOVSS.  Para vids fuera del rango de values (temporales
     * nuevos) usa la clase ya registrada en @c out.vreg_class. */
    auto vrt = [&](ir::IrValueId v) -> MOperand {
        RegClass cls = (v < out.vreg_class.size()) ? out.vreg_class[v]
                                                   : RegClass::GP;
        uint8_t w = 8;
        if (v < fn.values.size() && fn.values[v].type == ir::IrType::F32)
            w = 4;
        return MOperand::make_vreg(static_cast<uint32_t>(v), cls, w);
    };
    /* Crea un vreg temporal FP nuevo (XMM) para secuencias float (p.ej. el
     * MOVQ del float CONST a XMM, o el bitcast). */
    auto new_ftmp = [&]() -> ir::IrValueId {
        const ir::IrValueId id = out.vreg_count++;
        out.vreg_class.push_back(RegClass::FP);
        out.vreg_is_gc.push_back(0);
        return static_cast<ir::IrValueId>(id);
    };
    (void)new_ftmp;

    /* HOST_LEAF: emite los pseudo-ARG de una CALL/CALLIND/TAILCALL repartiendo
     * los args por CLASE (enteros -> arg_regs GP, floats -> arg_regs XMM),
     * con indice DENTRO DE SU CLASE (el ABI cuenta cada banco aparte).  El
     * operando float lleva clase FP (vrt) -> el rewrite lo manda a un XMM
     * arg_reg via MOVSD.  Devuelve false si algun arg excede los arg_regs de
     * su clase (paso por pila no soportado en v1). */
    auto emit_host_args = [&](const std::vector<ir::IrValueId> &operands,
                              std::vector<MInstr> &OO) -> bool {
        const size_t gmax =
            tri_sel.arg_regs[static_cast<size_t>(RegClass::GP)].size();
        const size_t fmax =
            fp_ok ? tri_sel.arg_regs[static_cast<size_t>(RegClass::FP)].size()
                  : 0;
        size_t gi_a = 0, fi_a = 0;
        for (size_t a = 0; a < operands.size(); ++a) {
            const ir::IrValueId av = operands[a];
            const bool is_f = fp_ok && av < fn.values.size() &&
                              ir_type_is_float(fn.values[av].type);
            if (is_f) {
                if (fi_a >= fmax) return false;
                OO.push_back(
                    MInstr::make_arg(static_cast<uint8_t>(fi_a), vrt(av)));
                ++fi_a;
            } else {
                if (gi_a >= gmax) return false;
                OO.push_back(
                    MInstr::make_arg(static_cast<uint8_t>(gi_a), vr(av)));
                ++gi_a;
            }
        }
        return true;
    };

    for (size_t b = 0; b < NB; ++b) {
        const ir::IrBlock &ib = fn.blocks[b];
        MBlock mb;
        mb.label_id = blbl[b];
        auto &O = mb.instrs;

        /* VM_ABI: al entrar, cargar cada parametro desde
         * proc->registers.regs[i+1] (regs[0] reservado al retorno). */
        if (vm && b == 0) {
            for (size_t i = 0; i < fn.params.size(); ++i)
                O.push_back(
                    MInstr::make_unary(MOp::MOV, vr(fn.params[i]),
                                       vm_reg_mem(static_cast<int>(i) + 1)));
        }

        /* HOST_LEAF: los params llegan en los arg_regs del ABI host.  Se
         * emite un def `MOV vr(param), <arg_reg fisico>` por param: (a) ancla
         * el intervalo del param en el entry (el regalloc reserva su home
         * desde la entrada, sin reusarlo antes del primer uso), y (b) es el
         * propio load.  El rewrite reconoce estos MOV lideres (vreg<-reg
         * fisico) y los emite como un PARALLEL-MOVE para romper los ciclos
         * arg_reg<->home (un MOV secuencial seria incorrecto si el home de un
         * param coincide con el arg_reg de otro).  Params mas alla de los
         * arg_regs (en pila) no se soportan en v1. */
        if (!vm && b == 0 && !fn.params.empty()) {
            const auto &areg =
                tri_sel.arg_regs[static_cast<size_t>(RegClass::GP)];
            const auto &fareg =
                tri_sel.arg_regs[static_cast<size_t>(RegClass::FP)];
            /* Indices SEPARADOS para enteros (GP) y floats (XMM): el ABI
             * (SysV/Win64) cuenta los arg_regs de cada clase de forma
             * INDEPENDIENTE (un float va al i-esimo XMM, no al i-esimo
             * registro global).  Cada MOV param-init es consumido por
             * emit_host_param_loads (FP-aware).  Params que exceden los
             * arg_regs de su clase (en pila) no se soportan en v1. */
            size_t gi_p = 0, fi_p = 0;
            for (size_t i = 0; i < fn.params.size(); ++i) {
                const ir::IrValueId pv = fn.params[i];
                const bool is_f =
                    fp_ok && pv < fn.values.size() &&
                    ir_type_is_float(fn.values[pv].type);
                if (is_f) {
                    if (fi_p >= fareg.size()) break; // float en pila: no v1
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, vrt(pv),
                        MOperand::make_reg(static_cast<MReg>(fareg[fi_p]), 8)));
                    ++fi_p;
                } else {
                    if (gi_p >= areg.size()) break; // int en pila: no v1
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, vrt(pv),
                        MOperand::make_reg(static_cast<MReg>(areg[gi_p]), 8)));
                    ++gi_p;
                }
            }
        }

        /* Emite las copias de PHI para una arista (b -> target): por cada
         * PHI del target, MOV phi_dst <- (arg cuyo pred == b). */
        auto emit_phi_copies = [&](ir::IrBlockId target) {
            const ir::IrBlock &tb = fn.blocks[target];
            for (const ir::IrInstr &p : tb.instrs) {
                if (p.op != ir::IrOp::PHI) continue;
                for (const ir::IrPhiArg &a : p.phi_args) {
                    if (a.block == static_cast<ir::IrBlockId>(b)) {
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(p.dst),
                                                       vr(a.value)));
                        break;
                    }
                }
            }
        };

        /* CMP diferido para fusionar compare-and-branch. */
        bool has_pend = false;
        MCond pend_cc = MCond::E;
        ir::IrValueId pend_dst = ir::IR_NO_VALUE, pend_a = 0, pend_b = 0;
        auto flush_pending = [&]() {
            if (!has_pend) return;
            O.push_back(mk_cmp(pend_a, pend_b));
            O.push_back(MInstr::make_unary(MOp::MOV, vr(pend_dst),
                                           MOperand::make_imm32(0)));
            O.push_back(mk_setcc(pend_dst, pend_cc));
            has_pend = false;
        };

        /* Solo-LSP (vista "Godbolt"): estampado diferido de source_line en
         * las MInstr de este bloque.  Cada IR-op emite a @c O; snapshot
         * ANTES de bajarla y estampamos al inicio de la op siguiente (y al
         * cerrar el bucle), sobreviviendo a los muchos @c continue.  OFF por
         * defecto -> cero efecto en el codegen AOT de produccion. */
        size_t lm_before = SIZE_MAX; // SIZE_MAX = nada pendiente
        uint32_t lm_line = 0;
        uint32_t lm_ir_id = 0xFFFFFFFFu; // identidad de la op IR (solo-LSP)
        auto lm_flush = [&]() {
            if (!out.emit_line_map || lm_before == SIZE_MAX) return;
            for (size_t k = lm_before; k < O.size(); ++k) {
                if (lm_line != 0 && O[k].source_pc == 0)
                    O[k].source_pc = lm_line;
                if (O[k].ir_id == 0xFFFFFFFFu) O[k].ir_id = lm_ir_id;
            }
            lm_before = SIZE_MAX;
        };

        for (const ir::IrInstr &in : ib.instrs) {
            MOp mop;
            MCond cc;
            // Estampar la op anterior (sobrevive a `continue`) + snapshot.
            lm_flush();
            if (out.emit_line_map) {
                lm_before = O.size();
                lm_line = in.source_line;
                /* Identidad estable de la op IR = block_index*65536 + pos. */
                size_t pos = static_cast<size_t>(&in - ib.instrs.data());
                lm_ir_id = static_cast<uint32_t>(b * 65536u + pos);
            }
            if (in.op == ir::IrOp::PHI) continue; // resuelto via copias

            if (cmp_cond(in.op, cc)) {
                flush_pending();
                if (in.operands.size() != 2) return false;
                /* Diferir: quiza se fusione con el BR_COND siguiente. */
                has_pend = true;
                pend_cc = cc;
                pend_dst = in.dst;
                pend_a = in.operands[0];
                pend_b = in.operands[1];
                continue;
            }

            switch (in.op) {
            case ir::IrOp::NOP: break;

            /* ADTs (markers semanticos puros, sin codegen): la
             * construccion/dispatch real lo emite la secuencia
             * ALLOCA + STORE tag + STOREs / LOAD + cmp + br que el
             * frontend genera ANTES/DESPUES del marker (ops ya
             * soportados por el vreg).  El ir_emitter los trata como
             * no-op (solo comentario); aqui igual.  No producen MInstr
             * -> el interval builder nunca los ve. */
            case ir::IrOp::MAKE_VARIANT: break;
            case ir::IrOp::MATCH_VARIANT: break;
            /* MAKE_CLOSURE: marker semantico puro (idem MAKE_VARIANT).
             * La construccion real (ALLOCA env + STOREs + ALLOCA fv +
             * STORE fn/env) la emite el frontend ANTES/DESPUES; el
             * marker no produce codigo.  No genera MInstr. */
            case ir::IrOp::MAKE_CLOSURE: break;

            /* READ_VM_REG: %dst = proc->registers.regs[imm].  En VM_ABI
             * RBX = ProcessVM* -> LOAD directo de [rbx + regs_off + 8*N]
             * (mismo patron que la carga de params).  El tipo/GC-ness
             * del dst ya lo marca el IR (vreg_is_gc loop).  Solo valido
             * en VM_ABI (en HOST_LEAF RBX no es proc). */
            case ir::IrOp::READ_VM_REG: {
                flush_pending();
                if (in.dst == ir::IR_NO_VALUE) break;
                if (abi != AbiKind::VM || in.imm > 15) {
                    vreg_dbg(fn.name.c_str(), "read_vm_reg");
                    return false;
                }
                O.push_back(
                    MInstr::make_unary(MOp::MOV, vr(in.dst),
                                       vm_reg_mem(static_cast<int>(in.imm))));
                break;
            }

            case ir::IrOp::CONST: {
                flush_pending();
                if (in.dst < v_is_const.size()) { // recordar para shifts
                    v_is_const[in.dst] = 1;
                    v_const[in.dst] = static_cast<int64_t>(in.imm);
                }
                /* Float CONST (Phase AOT C1 float): @c in.imm son los bits
                 * IEEE (f64 los 64 bits; f32 los 32 bajos).  Se cargan a un
                 * GP temporal (imm32/imm64) y se bitcastean al XMM dst via
                 * MOVQ_GP_XMM.  El rewrite resuelve el GP temp y el XMM dst
                 * (incluido el spill).  Mejora futura: lea de .rodata. */
                if (fp_ok && ir_type_is_float(in.type)) {
                    const ir::IrValueId t = new_tmp();
                    const int64_t fs = static_cast<int64_t>(in.imm);
                    if (fs >= INT32_MIN && fs <= INT32_MAX) {
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, vr(t),
                            MOperand::make_imm32(static_cast<int32_t>(fs))));
                    } else {
                        const uint32_t idx = out.intern_imm64(in.imm);
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, vr(t), MOperand::make_imm64_idx(idx)));
                    }
                    O.push_back(MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                                   vrt(in.dst), vr(t)));
                    break;
                }
                const int64_t s = static_cast<int64_t>(in.imm);
                if (s >= INT32_MIN && s <= INT32_MAX) {
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, vr(in.dst),
                        MOperand::make_imm32(static_cast<int32_t>(s))));
                } else {
                    const uint32_t idx = out.intern_imm64(in.imm);
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, vr(in.dst), MOperand::make_imm64_idx(idx)));
                }
                break;
            }

            case ir::IrOp::ADD:
            case ir::IrOp::SUB:
            case ir::IrOp::MUL:
            case ir::IrOp::AND:
            case ir::IrOp::OR:
            case ir::IrOp::XOR: {
                flush_pending();
                if (in.operands.size() != 2) return false;
                (void)bin_mop(in.op, mop);
                O.push_back(MInstr::make_binary(
                    mop, vr(in.dst), vr(in.operands[0]), vr(in.operands[1])));
                break;
            }

            /* FP arith binaria (Phase AOT C1 float).  El tipo del dst decide
             * f32 (SS) vs f64 (SD).  Forma 3-op (dst, a, b); el rewrite la
             * legaliza a 2-address (mov dst,a + OP dst,b).  fp_ok requerido
             * (en otro caso -> fallback). */
            case ir::IrOp::FADD:
            case ir::IrOp::FSUB:
            case ir::IrOp::FMUL:
            case ir::IrOp::FDIV: {
                flush_pending();
                if (!fp_ok) {
                    vreg_dbg(fn.name.c_str(), ir::ir_op_name(in.op));
                    return false;
                }
                if (in.operands.size() != 2 || in.dst == ir::IR_NO_VALUE)
                    return false;
                const bool is_f32 = (in.type == ir::IrType::F32);
                MOp fop;
                switch (in.op) {
                case ir::IrOp::FADD:
                    fop = is_f32 ? MOp::ADDSS : MOp::ADDSD;
                    break;
                case ir::IrOp::FSUB:
                    fop = is_f32 ? MOp::SUBSS : MOp::SUBSD;
                    break;
                case ir::IrOp::FMUL:
                    fop = is_f32 ? MOp::MULSS : MOp::MULSD;
                    break;
                default:
                    fop = is_f32 ? MOp::DIVSS : MOp::DIVSD;
                    break;
                }
                O.push_back(MInstr::make_binary(fop, vrt(in.dst),
                                                vrt(in.operands[0]),
                                                vrt(in.operands[1])));
                break;
            }

            /* FSQRT: dst = sqrt(src) (SD/SS). */
            case ir::IrOp::FSQRT: {
                flush_pending();
                if (!fp_ok) {
                    vreg_dbg(fn.name.c_str(), "fsqrt");
                    return false;
                }
                if (in.operands.size() != 1 || in.dst == ir::IR_NO_VALUE)
                    return false;
                const MOp fop =
                    (in.type == ir::IrType::F32) ? MOp::SQRTSS : MOp::SQRTSD;
                O.push_back(MInstr::make_unary(fop, vrt(in.dst),
                                               vrt(in.operands[0])));
                break;
            }

            /* FNEG / FABS: manipulan el bit de signo via una mascara en XMM.
             * La mascara se construye en un GP temp y se mueve a un XMM temp
             * (MOVQ_GP_XMM); luego XORPS (neg = flip bit signo, mascara =
             * signbit) o ANDPS (abs = clear bit signo, mascara = ~signbit).
             * f64: bit 63 (0x8000...0000); f32: bit 31 (0x80000000).  Ambos
             * comparten la misma maquinaria (3-op pre-legalizado por el
             * rewrite), cambiando solo la mascara y la op packed. */
            case ir::IrOp::FNEG:
            case ir::IrOp::FABS: {
                flush_pending();
                if (!fp_ok) {
                    vreg_dbg(fn.name.c_str(),
                             in.op == ir::IrOp::FABS ? "fabs" : "fneg");
                    return false;
                }
                if (in.operands.size() != 1 || in.dst == ir::IR_NO_VALUE)
                    return false;
                const bool is_f32 = (in.type == ir::IrType::F32);
                const bool is_abs = (in.op == ir::IrOp::FABS);
                /* FNEG: signbit (XOR para invertir).  FABS: ~signbit (AND para
                 * limpiar). */
                const uint64_t signbit =
                    is_f32 ? 0x80000000ull : 0x8000000000000000ull;
                const uint64_t mask = is_abs ? ~signbit : signbit;
                const ir::IrValueId gpm = new_tmp();
                const uint32_t midx = out.intern_imm64(mask);
                O.push_back(MInstr::make_unary(MOp::MOV, vr(gpm),
                                               MOperand::make_imm64_idx(midx)));
                const ir::IrValueId xm = new_ftmp();
                O.push_back(
                    MInstr::make_unary(MOp::MOVQ_GP_XMM, vrt(xm), vr(gpm)));
                /* dst = src (XOR|AND) mask (forma 3-op; el rewrite legaliza). */
                O.push_back(MInstr::make_binary(is_abs ? MOp::ANDPS : MOp::XORPS,
                                                vrt(in.dst),
                                                vrt(in.operands[0]), vrt(xm)));
                break;
            }

            /* Conversiones int <-> float (Phase AOT C1 float).  CVTSI2SD/SS
             * (entero signed -> float) / CVTTSD2SI/SS (float -> entero
             * truncado).  UITOF/FTOUI (unsigned) reusan la misma instr en v1
             * (correcto para valores que caben en i63; el caso > 2^63 se
             * documenta -- raro). */
            case ir::IrOp::ITOF:
            case ir::IrOp::UITOF: {
                flush_pending();
                if (!fp_ok) {
                    vreg_dbg(fn.name.c_str(), "itof");
                    return false;
                }
                if (in.operands.size() != 1 || in.dst == ir::IR_NO_VALUE)
                    return false;
                const MOp cop =
                    (in.type == ir::IrType::F32) ? MOp::CVTSI2SS : MOp::CVTSI2SD;
                O.push_back(MInstr::make_unary(cop, vrt(in.dst),
                                               vr(in.operands[0])));
                break;
            }
            case ir::IrOp::FTOI:
            case ir::IrOp::FTOUI: {
                flush_pending();
                if (!fp_ok) {
                    vreg_dbg(fn.name.c_str(), "ftoi");
                    return false;
                }
                if (in.operands.size() != 1 || in.dst == ir::IR_NO_VALUE)
                    return false;
                const ir::IrType st = fn.values[in.operands[0]].type;
                const MOp cop =
                    (st == ir::IrType::F32) ? MOp::CVTTSS2SI : MOp::CVTTSD2SI;
                O.push_back(MInstr::make_unary(cop, vr(in.dst),
                                               vrt(in.operands[0])));
                break;
            }
            case ir::IrOp::F32TOF64: {
                flush_pending();
                if (!fp_ok) {
                    vreg_dbg(fn.name.c_str(), "f32tof64");
                    return false;
                }
                if (in.operands.size() != 1 || in.dst == ir::IR_NO_VALUE)
                    return false;
                O.push_back(MInstr::make_unary(MOp::CVTSS2SD, vrt(in.dst),
                                               vrt(in.operands[0])));
                break;
            }
            case ir::IrOp::F64TOF32: {
                flush_pending();
                if (!fp_ok) {
                    vreg_dbg(fn.name.c_str(), "f64tof32");
                    return false;
                }
                if (in.operands.size() != 1 || in.dst == ir::IR_NO_VALUE)
                    return false;
                O.push_back(MInstr::make_unary(MOp::CVTSD2SS, vrt(in.dst),
                                               vrt(in.operands[0])));
                break;
            }

            /* FCMP_* (Phase AOT C1 float): UCOMISD/UCOMISS + SETcc.  El bool
             * resultante (0/1 en GP) lo consume un BR_COND (TEST + Jcc).
             * UCOMISD setea CF/ZF/PF: para ordenadas (no-NaN) el mapeo es:
             *   EQ -> setz (ZF=1 y PF=0; NaN da PF=1 -> tratamos como != )
             *   NE -> setnz | parity
             *   GT -> seta (CF=0,ZF=0)   GE -> setae (CF=0)
             *   LT -> setb (CF=1)        LE -> setbe
             * UCOMISD compara dst-vs-src1 (a,b): a>b -> CF=0,ZF=0.  Para
             * LT/LE invertimos el orden de operandos (b,a) y usamos a>b /
             * a>=b, evitando la ambiguedad de NaN en below.  v1: semantica
             * "ordered" simple (NaN -> false en todas salvo NE). */
            case ir::IrOp::FCMP_EQ:
            case ir::IrOp::FCMP_NE:
            case ir::IrOp::FCMP_LT:
            case ir::IrOp::FCMP_GT:
            case ir::IrOp::FCMP_LE:
            case ir::IrOp::FCMP_GE: {
                flush_pending();
                if (!fp_ok) {
                    vreg_dbg(fn.name.c_str(), ir::ir_op_name(in.op));
                    return false;
                }
                if (in.operands.size() != 2 || in.dst == ir::IR_NO_VALUE)
                    return false;
                const ir::IrType st = fn.values[in.operands[0]].type;
                const MOp ucmp =
                    (st == ir::IrType::F32) ? MOp::UCOMISS : MOp::UCOMISD;
                /* Elegir orden de operandos + condicion para usar siempre las
                 * condiciones "above/above-equal" (CF/ZF sin ambiguedad de
                 * NaN en el lado below).  a<b  <=>  b>a ; a<=b <=> b>=a. */
                ir::IrValueId ca = in.operands[0], cb = in.operands[1];
                MCond cc;
                switch (in.op) {
                case ir::IrOp::FCMP_EQ: cc = MCond::E; break;
                case ir::IrOp::FCMP_NE: cc = MCond::NE; break;
                case ir::IrOp::FCMP_GT: cc = MCond::A; break;
                case ir::IrOp::FCMP_GE: cc = MCond::AE; break;
                case ir::IrOp::FCMP_LT:
                    cc = MCond::A;
                    std::swap(ca, cb);
                    break;
                default: /* FCMP_LE */
                    cc = MCond::AE;
                    std::swap(ca, cb);
                    break;
                }
                /* UCOMISD a, b : dst=a (operando 1), src1=b.  El rewrite
                 * materializa ambos a XMM si estan spilled. */
                O.push_back(MInstr::make_unary(ucmp, vrt(ca), vrt(cb)));
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               MOperand::make_imm32(0)));
                O.push_back(mk_setcc(in.dst, cc));
                break;
            }
            /* DIV/MOD enteros (signed) via pseudo DIVMOD_V (opt-in
             * VESTA_JIT_VREG_IDIV mientras se valida).  El rewrite lo
             * expande a idiv usando R11 para el divisor (sin aliasing).
             * variant: 0 = cociente (DIV), 1 = resto (MOD). */
            case ir::IrOp::DIV:
            case ir::IrOp::MOD: {
                flush_pending();
                if (!jit_vreg_idiv()) {
                    vreg_dbg(fn.name.c_str(),
                             in.op == ir::IrOp::DIV ? "div" : "mod");
                    return false; // gated off -> slots
                }
                if (in.operands.size() != 2 || in.dst == ir::IR_NO_VALUE)
                    return false;
                MInstr dm{};
                dm.op = MOp::DIVMOD_V;
                dm.dst = vr(in.dst);
                dm.src1 = vr(in.operands[0]); // dividendo
                dm.src2 = vr(in.operands[1]); // divisor
                dm.variant = (in.op == ir::IrOp::MOD) ? 1u : 0u;
                O.push_back(dm);
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
            case ir::IrOp::SHL:
            case ir::IrOp::SHR:
            case ir::IrOp::SAR: {
                flush_pending();
                if (in.operands.size() != 2) return false;
                const ir::IrValueId amt_v = in.operands[1];
                if (amt_v >= v_is_const.size() || !v_is_const[amt_v]) {
                    vreg_dbg(fn.name.c_str(), "shift-var");
                    return false;
                }
                const int32_t amt = static_cast<int32_t>(v_const[amt_v] & 63);
                const MOp mop = (in.op == ir::IrOp::SHL)   ? MOp::SHL
                                : (in.op == ir::IrOp::SHR) ? MOp::SHR
                                                           : MOp::SAR;
                O.push_back(MInstr::make_binary(mop, vr(in.dst),
                                                vr(in.operands[0]),
                                                MOperand::make_imm32(amt)));
                break;
            }

            /* Math intrinsics como IrOps dedicados (el frontend baja
             * imin/imax/ilog2/clz/... a estos, NO a CALLN vmath_*).
             * Inline directo (principio "JIT inline > runtime"); misma
             * secuencia que el slot selector. */
            case ir::IrOp::IABS: { /* |a| = (a^(a>>63)) - (a>>63) */
                flush_pending();
                if (in.dst == ir::IR_NO_VALUE || in.operands.size() != 1)
                    return false;
                const ir::IrValueId t = new_tmp();
                O.push_back(
                    MInstr::make_unary(MOp::MOV, vr(t), vr(in.operands[0])));
                O.push_back(MInstr::make_binary(MOp::SAR, vr(t), vr(t),
                                                MOperand::make_imm32(63)));
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               vr(in.operands[0])));
                O.push_back(MInstr::make_binary(MOp::XOR, vr(in.dst),
                                                vr(in.dst), vr(t)));
                O.push_back(MInstr::make_binary(MOp::SUB, vr(in.dst),
                                                vr(in.dst), vr(t)));
                break;
            }
            case ir::IrOp::IMIN:
            case ir::IrOp::IMAX:
            case ir::IrOp::IMINU:
            case ir::IrOp::IMAXU: {
                /* dst = a; cmp a,b; cmov<cc> dst, b.  cc: IMIN->G,
                 * IMAX->L, IMINU->A, IMAXU->B (signed vs unsigned). */
                flush_pending();
                if (in.dst == ir::IR_NO_VALUE || in.operands.size() != 2)
                    return false;
                const ir::IrValueId a = in.operands[0], b = in.operands[1];
                const MCond cc = (in.op == ir::IrOp::IMIN)    ? MCond::G
                                 : (in.op == ir::IrOp::IMAX)  ? MCond::L
                                 : (in.op == ir::IrOp::IMINU) ? MCond::A
                                                              : MCond::B;
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst), vr(a)));
                O.push_back(mk_cmp(a, b));
                MInstr cm;
                cm.op = MOp::CMOVCC;
                cm.variant = static_cast<uint8_t>(cc);
                cm.dst = vr(in.dst);
                cm.src1 = vr(b);
                O.push_back(cm);
                break;
            }
            case ir::IrOp::ILOG2: { /* 63 - lzcnt(a); a==0 es UB (igual que
                                       slot) */
                flush_pending();
                if (in.dst == ir::IR_NO_VALUE || in.operands.size() != 1)
                    return false;
                O.push_back(MInstr::make_unary(MOp::LZCNT, vr(in.dst),
                                               vr(in.operands[0])));
                O.push_back(
                    MInstr::make_unary(MOp::NEG, vr(in.dst), vr(in.dst)));
                O.push_back(MInstr::make_binary(MOp::ADD, vr(in.dst),
                                                vr(in.dst),
                                                MOperand::make_imm32(63)));
                break;
            }
            case ir::IrOp::CLZ:
            case ir::IrOp::CTZ:
            case ir::IrOp::POPCNT: {
                flush_pending();
                if (in.dst == ir::IR_NO_VALUE || in.operands.size() != 1)
                    return false;
                const MOp mop = (in.op == ir::IrOp::CLZ)   ? MOp::LZCNT
                                : (in.op == ir::IrOp::CTZ) ? MOp::TZCNT
                                                           : MOp::POPCNT;
                O.push_back(
                    MInstr::make_unary(mop, vr(in.dst), vr(in.operands[0])));
                break;
            }
            case ir::IrOp::BYTESWAP: { /* mov dst, src; bswap dst */
                flush_pending();
                if (in.dst == ir::IR_NO_VALUE || in.operands.size() != 1)
                    return false;
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               vr(in.operands[0])));
                O.push_back(
                    MInstr::make_unary(MOp::BSWAP, vr(in.dst), vr(in.dst)));
                break;
            }
            case ir::IrOp::ROTL:
            case ir::IrOp::ROTR: {
                /* count CONSTANTE -> rol/ror dst, imm.  Variable -> CL
                 * (no soportado en vregs) -> fallback. */
                flush_pending();
                if (in.dst == ir::IR_NO_VALUE || in.operands.size() != 2)
                    return false;
                const ir::IrValueId cnt = in.operands[1];
                if (cnt >= v_is_const.size() || !v_is_const[cnt]) {
                    vreg_dbg(fn.name.c_str(), "rot-var");
                    return false;
                }
                const int32_t amt = static_cast<int32_t>(v_const[cnt] & 63);
                const MOp rop = (in.op == ir::IrOp::ROTL) ? MOp::ROL : MOp::ROR;
                O.push_back(MInstr::make_binary(rop, vr(in.dst),
                                                vr(in.operands[0]),
                                                MOperand::make_imm32(amt)));
                break;
            }

            /* Conversiones enteras: BITCAST (mismo ancho -> MOV),
             * SEXT/ZEXT/CAST (extension via MOVSX/MOVZX).  Las
             * truncaciones y los floats caen a fallback por ahora. */
            case ir::IrOp::BITCAST: {
                flush_pending();
                if (in.operands.size() != 1) return false;
                /* BITCAST reinterpreta bits sin convertir.  Entre el banco GP
                 * y el XMM hay que MOVER los bits con MOVQ (no un MOV GP->GP).
                 * Caso clave: enum/optional con payload f64 -> el payload se
                 * guarda como i64 (bitcast f64->i64) y al destructurar se hace
                 * bitcast i64->f64; sin el MOVQ el FMUL leeria un XMM sin
                 * escribir -> resultado basura. */
                const ir::IrValueId src = in.operands[0];
                const bool dst_f = in.dst < fn.values.size() &&
                                   ir_type_is_float(fn.values[in.dst].type);
                const bool src_f = src < fn.values.size() &&
                                   ir_type_is_float(fn.values[src].type);
                if (dst_f != src_f && !fp_ok) {
                    /* bitcast GP<->FP pero el target no soporta float -> no
                     * podemos emitir MOVQ; fallback. */
                    vreg_dbg(fn.name.c_str(), "bitcast-fp-no-fpok");
                    return false;
                }
                if (dst_f && !src_f) {
                    O.push_back(MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                                   vrt(in.dst), vr(src)));
                } else if (!dst_f && src_f) {
                    O.push_back(MInstr::make_unary(MOp::MOVQ_XMM_GP,
                                                   vr(in.dst), vrt(src)));
                } else if (dst_f && src_f) {
                    O.push_back(MInstr::make_unary(MOp::MOV, vrt(in.dst),
                                                   vrt(src)));
                } else {
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                                   vr(src)));
                }
                break;
            }
            case ir::IrOp::ZEXT:
            case ir::IrOp::SEXT:
            case ir::IrOp::CAST:
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
                    MOperand d = vr(in.dst);
                    d.width = static_cast<uint8_t>(w);
                    MOperand s = vr(in.operands[0]);
                    s.width = static_cast<uint8_t>(w);
                    O.push_back(MInstr::make_unary(MOp::MOV, d, s));
                };
                /* MOVSX/MOVZX dst64 <- src<srcw>. */
                auto ext = [&](MOp mop, int srcw) {
                    const MOperand s = MOperand::make_vreg(
                        static_cast<uint32_t>(in.operands[0]), RegClass::GP,
                        static_cast<uint8_t>(srcw));
                    O.push_back(MInstr::make_unary(mop, vr(in.dst), s));
                };
                if (db == sb) {
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, vr(in.dst),
                        vr(in.operands[0]))); // copia de bits
                } else if (db > sb) {
                    /* EXTENSION (zext/sext). */
                    const bool sign =
                        (in.op == ir::IrOp::SEXT) ||
                        (in.op == ir::IrOp::CAST && ir_type_signed(st));
                    if (sb == 1 || sb == 2)
                        ext(sign ? MOp::MOVSX : MOp::MOVZX, sb);
                    else if (sign)
                        ext(MOp::MOVSX, 4); // i32->i64 (MOVSXD)
                    else
                        mov_w(4); // u32->u64 zero-ext
                } else {
                    /* TRUNCACION (db < sb): el signo lo da el DESTINO. */
                    const bool sign = ir_type_signed(dt);
                    if (db == 4) {
                        if (sign)
                            ext(MOp::MOVSX, 4); // i32: 32 bajos sign-ext
                        else
                            mov_w(4); // u32: 32 bajos zero-ext
                    } else {          // db == 1 || db == 2
                        ext(sign ? MOp::MOVSX : MOp::MOVZX, db);
                    }
                }
                break;
            }

            case ir::IrOp::BR: {
                flush_pending();
                const ir::IrBlockId t = in.target_block;
                emit_phi_copies(t); // pred 1-succ: seguro
                O.push_back(MInstr::make_jmp(blbl[t]));
                mb.succ_a = static_cast<MBlockId>(t);
                break;
            }

            case ir::IrOp::BR_COND: {
                if (in.operands.size() != 1) return false;
                const ir::IrBlockId tt = in.target_block; // true
                const ir::IrBlockId tf = in.false_block;  // false
                /* Tras el critical-edge splitting, ningun target de un
                 * BR_COND de 2 vias tiene PHIs (los puentes los absorben).
                 * Defensa: si por construccion quedara uno (no deberia),
                 * caer a fallback en vez de emitir copias incorrectas. */
                if (tt != tf && (block_has_phi(fn.blocks[tt]) ||
                                 block_has_phi(fn.blocks[tf]))) {
                    vreg_dbg(fn.name.c_str(), "br_cond-phi-target");
                    return false;
                }
                const ir::IrValueId cond = in.operands[0];

                if (has_pend && pend_dst == cond &&
                    vreg_count_uses(fn, pend_dst) == 1) {
                    /* FUSION: CMP a,b + Jcc(cc) true + JMP false.  Solo si el
                     * bool del CMP se usa UNICAMENTE en este BR_COND (si no,
                     * hay que materializarlo via SETcc para el otro uso). */
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
                    /* Float return (Phase AOT C1, HOST_LEAF): el valor va a
                     * XMM0 (ret_reg[FP]).  El MOV XMM0 <- vreg_fp lo enruta el
                     * rewrite a MOVSD (is_fp_operand).  En VM_ABI float aun no
                     * se soporta (fp_ok=false -> no llegan ops float aqui). */
                    const ir::IrValueId rv = in.operands[0];
                    if (!vm && fp_ok && rv < fn.values.size() &&
                        ir_type_is_float(fn.values[rv].type)) {
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, MOperand::make_reg(MReg::XMM0, 8),
                            vrt(rv)));
                        O.push_back(MInstr::make_ret());
                        break;
                    }
                    /* VM_ABI: escribir el resultado en
                     * proc->registers.regs[0] ([rbx+off]); host leaf:
                     * dejarlo en RAX. */
                    const MOperand dst =
                        vm ? vm_reg_mem(0) : MOperand::make_reg(MReg::RAX, 8);
                    O.push_back(
                        MInstr::make_unary(MOp::MOV, dst, vr(in.operands[0])));
                } else if (vm) {
                    /* RET void en VM_ABI: regs[0] es el "exit code"
                     * observable de main.  Sin esto quedaria con basura
                     * del ultimo CALL VM_ABI (p.ej. __new_X deja el ptr
                     * del objeto).  El interp/selector dan 0 aqui (el
                     * ultimo CALLN deja 0 en R0/RAX); escribimos 0
                     * explicito -> exit-code determinista + paridad con
                     * el interp en `void main`.  Reusa el patron seguro
                     * mem<-vreg del RET con operando (un vreg temporal
                     * con 0; el rewrite ya sabe materializarlo). */
                    const ir::IrValueId zero = new_tmp();
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(zero),
                                                   MOperand::make_imm32(0)));
                    O.push_back(
                        MInstr::make_unary(MOp::MOV, vm_reg_mem(0), vr(zero)));
                }
                O.push_back(MInstr::make_ret());
                break;
            }

            /* Fase 2: GETSTATIC/SETSTATIC = acceso directo (sin runtime
             * call) a `cls->static_data + offset`.  cls->static_data es
             * un host_ptr (offset 96 en ClassInfo); el valor vive en ese
             * bloque host.  Dos loads/un store encadenados (el MEM con
             * base-vreg no se soporta pre-regalloc -> ADD + LOAD disp0).
             * El frontend ya hace el truncate/sign-extend del valor. */
            case ir::IrOp::GETSTATIC: {
                flush_pending();
                if (in.dst == ir::IR_NO_VALUE || in.operands.empty())
                    return false;
                const int32_t off = static_cast<int32_t>(in.imm);
                const ir::IrValueId t_cls = new_tmp();
                O.push_back(MInstr::make_unary(MOp::MOV, vr(t_cls),
                                               vr(in.operands[0])));
                O.push_back(MInstr::make_binary(
                    MOp::ADD, vr(t_cls), vr(t_cls),
                    MOperand::make_imm32(VESTA_CLASSINFO_STATIC_DATA_OFFSET)));
                const ir::IrValueId sd = new_tmp();
                O.push_back(MInstr::make_load(vr(sd), vr(t_cls), 8, false));
                if (off != 0)
                    O.push_back(MInstr::make_binary(MOp::ADD, vr(sd), vr(sd),
                                                    MOperand::make_imm32(off)));
                O.push_back(MInstr::make_load(vr(in.dst), vr(sd), 8, false));
                break;
            }
            case ir::IrOp::SETSTATIC: {
                flush_pending();
                if (in.operands.size() < 2) return false;
                const int32_t off = static_cast<int32_t>(in.imm);
                const ir::IrValueId t_cls = new_tmp();
                O.push_back(MInstr::make_unary(MOp::MOV, vr(t_cls),
                                               vr(in.operands[0])));
                O.push_back(MInstr::make_binary(
                    MOp::ADD, vr(t_cls), vr(t_cls),
                    MOperand::make_imm32(VESTA_CLASSINFO_STATIC_DATA_OFFSET)));
                const ir::IrValueId sd = new_tmp();
                O.push_back(MInstr::make_load(vr(sd), vr(t_cls), 8, false));
                if (off != 0)
                    O.push_back(MInstr::make_binary(MOp::ADD, vr(sd), vr(sd),
                                                    MOperand::make_imm32(off)));
                O.push_back(MInstr::make_store(vr(sd), vr(in.operands[1]), 8));
                break;
            }

            /* ALLOCA host (auto-promote, no escapa): reserva en el frame
             * JIT -> dst = host_ptr.  ALLOCA-vm (host_alloca=false): el
             * ptr escapa (necesita vaddr valido para el runtime) ->
             * reservar en el VM stack del proceso via ALLOCA_VM (el
             * prologue/epilogue salva/restaura el VM-RSP). */
            case ir::IrOp::ALLOCA: {
                flush_pending();
                /* Phase AS inc.5: ALLOCA de un binding register() ->
                 * NO emite host-slot; el vreg ya esta precoloreado a su
                 * registro fisico (set_vreg_fixed) y representa el VALOR
                 * directamente.  Los STORE/LOAD a su alloca se colapsan a
                 * MOVs (abajo). */
                if (in.dst != ir::IR_NO_VALUE && in.dst < binding_phys.size() &&
                    binding_phys[in.dst] >= 0)
                    break;
                const uint64_t size = in.imm;
                if (size == 0 || size > 65536) { // sanity (frame chico)
                    vreg_dbg(fn.name.c_str(), "alloca-size");
                    return false;
                }
                if (!in.host_alloca) {
                    O.push_back(MInstr::make_alloca_vm(
                        vr(in.dst), static_cast<uint32_t>(size)));
                    break;
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
                /* Phase AS inc.5: LOAD desde el alloca de un binding ->
                 * leer el output del inline-asm: MOV dst <- vbind. */
                if (in.dst != ir::IR_NO_VALUE &&
                    in.operands[0] < binding_phys.size() &&
                    binding_phys[in.operands[0]] >= 0) {
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                                   vr(in.operands[0])));
                    break;
                }
                if (ir_type_is_float(in.type)) {
                    vreg_dbg(fn.name.c_str(), "load-float");
                    return false;
                }
                const int w = ir_type_bytes(in.type);
                const bool sgn = ir_type_signed(in.type);
                /* AOT (HOST_LEAF): NO hay vm_mem -> toda direccion es host
                 * (el str_lit_addr/.rodata, malloc, alloca son host_ptr
                 * reales).  Solo el VM_ABI traduce vaddr -> host via
                 * LOAD_VM; en bare emitimos LOAD host directo. */
                if (vm && !fn.values[in.operands[0]].is_host_ptr) {
                    /* vm_mem (vaddr): page-cache inline + fallback al
                     * runtime vrt_vm_read_u<w> (la direccion se hornea
                     * en imm64_pool; el rewrite expande POST-regalloc). */
                    uint64_t fn_addr = 0;
                    switch (w) {
                    case 1: fn_addr = ent.vm_read_u8; break;
                    case 2: fn_addr = ent.vm_read_u16; break;
                    case 4: fn_addr = ent.vm_read_u32; break;
                    default: fn_addr = ent.vm_read_u64; break;
                    }
                    if (fn_addr == 0) {
                        vreg_dbg(fn.name.c_str(), "load-vm(no-rt)");
                        return false;
                    }
                    const uint32_t fidx = out.intern_imm64(fn_addr);
                    O.push_back(MInstr::make_load_vm(
                        vr(in.dst), vr(in.operands[0]), static_cast<uint8_t>(w),
                        sgn, fidx));
                    break;
                }
                /* host_ptr: LOAD directo (commit 7).  u32 unsigned -> el
                 * rewrite emite `mov r32,[mem]` (zero-extend por hardware). */
                O.push_back(MInstr::make_load(vr(in.dst), vr(in.operands[0]),
                                              static_cast<uint8_t>(w), sgn));
                break;
            }
            case ir::IrOp::STORE: {
                flush_pending();
                if (in.operands.size() != 2) return false; // [0]=val [1]=ptr
                /* Phase AS inc.5: STORE al alloca de un binding ->
                 * cargar el input del inline-asm: MOV vbind <- val. */
                if (in.operands[1] < binding_phys.size() &&
                    binding_phys[in.operands[1]] >= 0) {
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(in.operands[1]),
                                                   vr(in.operands[0])));
                    break;
                }
                if (ir_type_is_float(in.type)) {
                    vreg_dbg(fn.name.c_str(), "store-float");
                    return false;
                }
                const int w = ir_type_bytes(in.type);
                /* AOT (HOST_LEAF): toda direccion es host -> STORE directo
                 * (ver nota en LOAD).  Solo VM_ABI usa STORE_VM. */
                if (vm && !fn.values[in.operands[1]].is_host_ptr) {
                    /* vm_mem (vaddr): page-cache inline + fallback al
                     * runtime vrt_vm_write_u<w>. */
                    uint64_t fn_addr = 0;
                    switch (w) {
                    case 1: fn_addr = ent.vm_write_u8; break;
                    case 2: fn_addr = ent.vm_write_u16; break;
                    case 4: fn_addr = ent.vm_write_u32; break;
                    default: fn_addr = ent.vm_write_u64; break;
                    }
                    if (fn_addr == 0) {
                        vreg_dbg(fn.name.c_str(), "store-vm(no-rt)");
                        return false;
                    }
                    const uint32_t fidx = out.intern_imm64(fn_addr);
                    O.push_back(MInstr::make_store_vm(
                        vr(in.operands[1]), vr(in.operands[0]),
                        static_cast<uint8_t>(w), fidx));
                    break;
                }
                O.push_back(MInstr::make_store(vr(in.operands[1]),
                                               vr(in.operands[0]),
                                               static_cast<uint8_t>(w)));
                break;
            }

            /* MEMCPY %dst_ptr, %src_ptr, %len -> `rep movsb` (memcpy x86
             * nativo, perf strings 2026-06-18).  REP MOVSB copia RCX bytes
             * desde [RSI] a [RDI].  Secuencia AUTO-CONTENIDA respecto al
             * regalloc: salvamos RSI/RDI/RCX con PUSH (por si tienen vregs
             * vivos a traves -- RSI/RDI son callee-saved asignables en
             * Win64, RCX caller-saved asignable) y los restauramos con POP.
             * Asi NO importa que asignacion fisica tengan los operandos.
             *
             * Orden de carga (sin colisiones):
             *   MOV R10, dst   ; R10/R11 = scratch del rewrite (NO asignables)
             *   MOV R11, src   ;   -> vr(dst)/vr(src) jamas estan en R10/R11
             *   PUSH RDI ; PUSH RSI ; PUSH RCX   (salvar fijos)
             *   MOV RCX, len   ; len desde su fisico (RSI/RDI/RCX aun intactos)
             *   MOV RDI, R10   ; dst
             *   MOV RSI, R11   ; src
             *   REP MOVSB      ; copia RCX bytes [RSI]->[RDI]
             *   POP RCX ; POP RSI ; POP RDI       (restaurar)
             * No es call-position: PUSH/POP preservan todo; el unico uso de
             * los vregs operandos es ANTES de clobear RSI/RDI/RCX. */
            case ir::IrOp::MEMCPY: {
                flush_pending();
                if (in.operands.size() != 3) return false;
                /* 1. dst/src a los scratch reservados (R10/R11): el rewrite
                 *    resuelve cada vr() a su fisico/spill; como el DESTINO es
                 *    un reg fisico, emite MOV reg,reg o MOV reg,[rbp-off]
                 *    directo (no recurre a R10/R11 como intermediario). */
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R10, 8),
                                               vr(in.operands[0]))); // dst
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R11, 8),
                                               vr(in.operands[1]))); // src
                /* 2. Salvar los fijos (pueden tener vregs vivos a traves). */
                O.push_back(MInstr::make_unary(
                    MOp::PUSH, MOperand::none(),
                    MOperand::make_reg(MReg::RDI, 8)));
                O.push_back(MInstr::make_unary(
                    MOp::PUSH, MOperand::none(),
                    MOperand::make_reg(MReg::RSI, 8)));
                O.push_back(MInstr::make_unary(
                    MOp::PUSH, MOperand::none(),
                    MOperand::make_reg(MReg::RCX, 8)));
                /* 3. len -> RCX (leido de su fisico ANTES de clobear los
                 *    fijos; RSI/RDI/RCX aun intactos en este punto). */
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::RCX, 8),
                                               vr(in.operands[2]))); // len
                /* 4. dst/src desde los scratch a los fijos. */
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::RDI, 8),
                    MOperand::make_reg(MReg::R10, 8)));
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::RSI, 8),
                    MOperand::make_reg(MReg::R11, 8)));
                /* 5. La copia. */
                O.push_back(MInstr::make_rep_movsb());
                /* 6. Restaurar los fijos (orden inverso). */
                O.push_back(MInstr::make_unary(
                    MOp::POP, MOperand::make_reg(MReg::RCX, 8),
                    MOperand::none()));
                O.push_back(MInstr::make_unary(
                    MOp::POP, MOperand::make_reg(MReg::RSI, 8),
                    MOperand::none()));
                O.push_back(MInstr::make_unary(
                    MOp::POP, MOperand::make_reg(MReg::RDI, 8),
                    MOperand::none()));
                break;
            }

            /* Phase AS inc.5: bloque de inline-asm nativo.  El cuerpo
             * (NASM Intel, ya con comptime-consts sustituidas por el
             * frontend) se ENSAMBLA a bytes via g_asm_backend; se emite
             * como INLINE_ASM_RAW (el encoder lo apendea verbatim).  Los
             * operandos del IrInstr son los alloca de los register()
             * (ya colapsados a vregs precoloreados); su clasificacion
             * in/out alimenta la liveness del AsmBlob. */
            case ir::IrOp::INLINE_ASM: {
                flush_pending();
                /* Phase NR @Naked: un asm{} SIN register() bindings (cuerpo
                 * de un ISR/stub) tiene @c asm_reg_bindings vacio -> antes
                 * caia por @c !has_inline_asm.  El unico requisito real es el
                 * backend de ensamblado; sin bindings, @c in.operands esta
                 * vacio y el blob no marca in/out vregs (correcto). */
                if (vex::g_asm_backend == nullptr) {
                    vreg_dbg(fn.name.c_str(), "inline-asm(no-backend)");
                    return false;
                }
                vex::AsmAssembleResult ar = vex::g_asm_backend->assemble(
                    in.func_name, vex::AsmArch::X86_64);
                if (!ar.ok || ar.bytes.empty()) {
                    vreg_dbg(fn.name.c_str(), "inline-asm(assemble-fail)");
                    return false;
                }
                AsmBlob blob;
                // Solo-inspeccion: mapear etiquetas internas + linea por instr
                // usando el contrato insn_offsets (offset de cada instruccion
                // en orden de fuente).  NUESTRO parser del texto NASM: una
                // instruccion por linea; las etiquetas ("name:") mapean al
                // offset de la instruccion SIGUIENTE.  Sin acoplar a Keystone.
                if (out.emit_line_map && !ar.insn_offsets.empty()) {
                    const std::string &body = in.func_name;
                    const uint32_t base_line = in.source_line;
                    size_t pos = 0;
                    int instr_idx = 0, body_line = 0;
                    while (pos <= body.size()) {
                        size_t nl = body.find('\n', pos);
                        std::string ln = body.substr(
                            pos, nl == std::string::npos ? std::string::npos
                                                         : nl - pos);
                        // recortar + quitar comentario (; o //)
                        size_t c = ln.find(';');
                        if (c != std::string::npos) ln.resize(c);
                        size_t cc = ln.find("//");
                        if (cc != std::string::npos) ln.resize(cc);
                        size_t b0 = ln.find_first_not_of(" \t");
                        size_t b1 = ln.find_last_not_of(" \t");
                        std::string t =
                            (b0 == std::string::npos) ? std::string()
                                                      : ln.substr(b0, b1 - b0 + 1);
                        // etiqueta inicial "name:" (posible "name: instr")
                        if (!t.empty()) {
                            size_t colon = t.find(':');
                            size_t sp = t.find_first_of(" \t");
                            if (colon != std::string::npos &&
                                (sp == std::string::npos || colon < sp)) {
                                std::string name = t.substr(0, colon);
                                if (instr_idx <
                                    (int)ar.insn_offsets.size())
                                    blob.labels.push_back(
                                        {ar.insn_offsets[instr_idx], name});
                                // resto tras "name:" puede ser una instruccion
                                size_t rest = t.find_first_not_of(
                                    " \t", colon + 1);
                                t = (rest == std::string::npos)
                                        ? std::string()
                                        : t.substr(rest);
                            }
                        }
                        if (!t.empty()) { // instruccion
                            if (instr_idx < (int)ar.insn_offsets.size()) {
                                blob.insn_lines.push_back(
                                    {ar.insn_offsets[instr_idx],
                                     base_line + 1 + body_line});
                                ++instr_idx;
                            }
                        }
                        ++body_line;
                        if (nl == std::string::npos) break;
                        pos = nl + 1;
                    }
                }
                blob.bytes = std::move(ar.bytes);
                for (ir::IrValueId opv : in.operands) {
                    if (opv >= binding_phys.size() || binding_phys[opv] < 0)
                        continue;
                    if (binding_is_in[opv])
                        blob.in_vregs.push_back(static_cast<uint32_t>(opv));
                    if (binding_is_out[opv])
                        blob.out_vregs.push_back(static_cast<uint32_t>(opv));
                    /* binding sin STORE ni LOAD (raro): tratar como in+out
                     * para que su intervalo cubra el asm y el pin se
                     * respete (conservador). */
                    if (!binding_is_in[opv] && !binding_is_out[opv]) {
                        blob.in_vregs.push_back(static_cast<uint32_t>(opv));
                        blob.out_vregs.push_back(static_cast<uint32_t>(opv));
                    }
                }
                blob.clobbers_mem = ((in.imm >> 4) & 1u) != 0;
                blob.clobbers_flags = ((in.imm >> 5) & 1u) != 0;
                // Phase AS inc.5e: registros fisicos clobbered (explicitos
                // del usuario + inferidos; asm_clobber_lists YA excluye los
                // regs ligados por register()).  El regalloc los excluye
                // para vregs NO-binding vivos a traves del asm -- cubre los
                // clobbers de callee-saved (r12-r15) que el call-position
                // (solo caller-saved) no protege.  asm_id en imm bits 8..31.
                //
                // Phase AS inc.5f: clobbers de registros RESERVADOS por el
                // wrapper (rbx = ProcessVM*, rbp = frame).  canon_gp_to_mreg
                // los rechaza (no son asignables), pero el asm SI los pisa
                // (p.ej. cpuid escribe ebx) -> hay que SALVARLOS y
                // RESTAURARLOS alrededor del bloque, o el wrapper corrompe
                // proc/el frame.  rsp clobbered NO es envolvible (las
                // push/pop usan rsp) -> fallback.
                bool save_rbx = false, save_rbp = false;
                {
                    const uint64_t asm_id = (in.imm >> 8) & 0xFFFFFFull;
                    if (asm_id < fn.asm_clobber_lists.size()) {
                        for (const auto &cn : fn.asm_clobber_lists[asm_id]) {
                            const std::string c = vex::asm_canonical_reg(cn);
                            const int phys = canon_gp_to_mreg(c);
                            if (phys >= 0) {
                                blob.clobbers.push_back(
                                    static_cast<uint8_t>(phys));
                            } else if (c == "rbx") {
                                save_rbx = true;
                            } else if (c == "rbp") {
                                save_rbp = true;
                            } else if (c == "rsp") {
                                vreg_dbg(fn.name.c_str(),
                                         "inline-asm(clobber-rsp)");
                                return false;
                            }
                        }
                    }
                }
                const uint32_t bidx = out.intern_asm_blob(std::move(blob));
                /* Salvar reservados clobbered ANTES del asm (push). */
                if (save_rbx) {
                    MInstr p;
                    p.op = MOp::PUSH;
                    p.src1 = MOperand::make_reg(MReg::RBX, 8);
                    O.push_back(p);
                }
                if (save_rbp) {
                    MInstr p;
                    p.op = MOp::PUSH;
                    p.src1 = MOperand::make_reg(MReg::RBP, 8);
                    O.push_back(p);
                }
                O.push_back(MInstr::make_inline_asm_raw(bidx));
                /* Restaurar en orden inverso (pop). */
                if (save_rbp) {
                    MInstr p;
                    p.op = MOp::POP;
                    p.dst = MOperand::make_reg(MReg::RBP, 8);
                    O.push_back(p);
                }
                if (save_rbx) {
                    MInstr p;
                    p.op = MOp::POP;
                    p.dst = MOperand::make_reg(MReg::RBX, 8);
                    O.push_back(p);
                }
                break;
            }

            /* GC_DEREF_HOST: dst = deref(handle).  INLINE del lookup
             * (principio "JIT inline > runtime"): replica
             * @c GcHeap::deref leyendo la HandleTable directamente, en
             * vez de un CALL a @c vrt_gc_deref (~6x: 30ns -> 5ns).
             * Es el op de field-access GC mas caliente.
             *
             * Semantica replicada (ver gc_heap.cpp::deref):
             *   if (h & SHARED_HANDLE_BIT) return shared_lookup(h);  // bit31
             *   if (h >= count_ || !data_[h].live) return nullptr;
             *   return data_[h].addr + sizeof(GcHeader);             // +8
             * con HandleEntry{ addr@0, live@8, stride 16 } (static_assert
             * en gc_heap.h) y HandleTable{ data_@0, count_@8 }.
             *
             * SEGURIDAD GC: dst es un host_ptr GC.  Si su intervalo
             * cruza el CALL del path shared, el commit 6 lo spillea a
             * slot + emite stackmap.  Por eso lo INICIALIZAMOS a 0
             * (null) al principio: si el GC corre durante ese CALL, el
             * slot contiene null (root ignorado), nunca basura. */
            case ir::IrOp::GC_DEREF_HOST: {
                flush_pending();
                if (!vm || ent.gc_deref == 0) {
                    vreg_dbg(fn.name.c_str(), "gc_deref(no-vm/no-addr)");
                    return false;
                }
                if (in.operands.size() != 1) return false;
                if (in.dst == ir::IR_NO_VALUE) break; // lookup sin uso: no-op

                /* A-B / diagnostico: enrutar al CALL vrt_gc_deref. */
                if (jit_no_inline_deref()) {
#if defined(_WIN32)
                    const MReg ca0 = MReg::RCX, ca1 = MReg::RDX;
#else
                    const MReg ca0 = MReg::RDI, ca1 = MReg::RSI;
#endif
                    O.push_back(MInstr::make_unary(MOp::MOV,
                                                   MOperand::make_reg(ca1, 8),
                                                   vr(in.operands[0])));
                    O.push_back(
                        MInstr::make_unary(MOp::MOV, MOperand::make_reg(ca0, 8),
                                           MOperand::make_reg(MReg::RBX, 8)));
                    O.push_back(
                        MInstr::make_call_abs(out.intern_imm64(ent.gc_deref)));
                    O.push_back(
                        MInstr::make_unary(MOp::MOV, vr(in.dst),
                                           MOperand::make_reg(MReg::RAX, 8)));
                    break;
                }

                const ir::IrValueId h = in.operands[0];
                const MLabelId Lsh = out.new_label();   // path shared
                const MLabelId Ldone = out.new_label(); // salida comun

                /* ORDEN: path local (comun) como FALLTHROUGH para que el
                 * hot path no pague un branch tomado; el path shared
                 * (raro) al final.  dst se INICIALIZA a 0 al principio:
                 * (a) es el resultado de los early-exits (fuera de rango
                 * / !live); (b) como su intervalo cruza el CALL del path
                 * shared, el commit 6 lo spillea a slot + stackmap -- con
                 * dst=0 ese slot es null (GC root ignorado), nunca basura.
                 * El spill (2 ops L1 por deref) resulto mas barato que el
                 * branch tomado del orden inverso en workloads memory-bound. */

                /* dst = 0 (null). */
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               MOperand::make_imm32(0)));

                /* h32 = handle normalizado a 32 bits (zero-extend).  El
                 * handle es u32; un productor podria dejar bits altos
                 * sucios en el reg de 64 -> shl 32 / shr 32 los limpia. */
                const ir::IrValueId h32 = new_tmp();
                O.push_back(MInstr::make_unary(MOp::MOV, vr(h32), vr(h)));
                O.push_back(MInstr::make_binary(MOp::SHL, vr(h32), vr(h32),
                                                MOperand::make_imm32(32)));
                O.push_back(MInstr::make_binary(MOp::SHR, vr(h32), vr(h32),
                                                MOperand::make_imm32(32)));

                /* if (h & SHARED_HANDLE_BIT) goto Lsh.  bit31 = h32 >> 31. */
                const ir::IrValueId t_sh = new_tmp();
                O.push_back(MInstr::make_unary(MOp::MOV, vr(t_sh), vr(h32)));
                O.push_back(MInstr::make_binary(MOp::SHR, vr(t_sh), vr(t_sh),
                                                MOperand::make_imm32(31)));
                O.push_back(mk_test(t_sh, t_sh));
                O.push_back(MInstr::make_jcc(MCond::NE, Lsh));

                /* --- path local (fallthrough) --- */
                /* base = proc->jit_handle_table  (HandleTable*). */
                const ir::IrValueId t_base = new_tmp();
                O.push_back(MInstr::make_unary(
                    MOp::MOV, vr(t_base),
                    MOperand::make_mem(MReg::RBX,
                                       VESTA_PROC_JIT_HANDLE_TABLE_OFFSET)));
                /* data_ = [base + 0]  (HandleEntry*). */
                const ir::IrValueId t_data = new_tmp();
                O.push_back(
                    MInstr::make_load(vr(t_data), vr(t_base), 8, false));
                /* count_ = low32 de [base + 8].  Cargamos 8 bytes
                 * ((cap_<<32)|count_) y aislamos los 32 bajos con
                 * shl 32 / shr 32 (un LOAD width-4 unsigned seria un
                 * MOVZX r/m32 invalido). */
                const ir::IrValueId t_b8 = new_tmp();
                O.push_back(MInstr::make_unary(MOp::MOV, vr(t_b8), vr(t_base)));
                O.push_back(MInstr::make_binary(MOp::ADD, vr(t_b8), vr(t_b8),
                                                MOperand::make_imm32(8)));
                const ir::IrValueId t_cnt = new_tmp();
                O.push_back(MInstr::make_load(vr(t_cnt), vr(t_b8), 8, false));
                O.push_back(MInstr::make_binary(MOp::SHL, vr(t_cnt), vr(t_cnt),
                                                MOperand::make_imm32(32)));
                O.push_back(MInstr::make_binary(MOp::SHR, vr(t_cnt), vr(t_cnt),
                                                MOperand::make_imm32(32)));
                /* if (h32 >= count_) goto Ldone  (dst sigue 0).  Unsigned. */
                O.push_back(mk_cmp(h32, t_cnt));
                O.push_back(MInstr::make_jcc(MCond::AE, Ldone));
                /* entry = data_ + h32 * 16. */
                const ir::IrValueId t_idx = new_tmp();
                O.push_back(MInstr::make_unary(MOp::MOV, vr(t_idx), vr(h32)));
                O.push_back(MInstr::make_binary(MOp::SHL, vr(t_idx), vr(t_idx),
                                                MOperand::make_imm32(4)));
                const ir::IrValueId t_entry = new_tmp();
                O.push_back(
                    MInstr::make_unary(MOp::MOV, vr(t_entry), vr(t_data)));
                O.push_back(MInstr::make_binary(MOp::ADD, vr(t_entry),
                                                vr(t_entry), vr(t_idx)));
                /* live = [entry + 8]  (byte).  if (!live) goto Ldone. */
                const ir::IrValueId t_la = new_tmp();
                O.push_back(
                    MInstr::make_unary(MOp::MOV, vr(t_la), vr(t_entry)));
                O.push_back(MInstr::make_binary(MOp::ADD, vr(t_la), vr(t_la),
                                                MOperand::make_imm32(8)));
                const ir::IrValueId t_live = new_tmp();
                O.push_back(MInstr::make_load(vr(t_live), vr(t_la), 1, false));
                O.push_back(mk_test(t_live, t_live));
                O.push_back(MInstr::make_jcc(MCond::E, Ldone));
                /* dst = [entry + 0] (addr) + sizeof(GcHeader)=8. */
                O.push_back(
                    MInstr::make_load(vr(in.dst), vr(t_entry), 8, false));
                O.push_back(MInstr::make_binary(
                    MOp::ADD, vr(in.dst), vr(in.dst), MOperand::make_imm32(8)));
                O.push_back(MInstr::make_jmp(Ldone));

                /* --- path shared (raro): fallback CALL vrt_gc_deref(proc, h)
                 * --- */
                O.push_back(MInstr::make_label_def(Lsh));
#if defined(_WIN32)
                const MReg sa0 = MReg::RCX, sa1 = MReg::RDX;
#else
                const MReg sa0 = MReg::RDI, sa1 = MReg::RSI;
#endif
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(sa1, 8),
                                               vr(h))); // arg1 = handle
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(sa0, 8),
                    MOperand::make_reg(MReg::RBX, 8))); // arg0 = proc
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.gc_deref)));
                O.push_back(MInstr::make_unary(
                    MOp::MOV, vr(in.dst),
                    MOperand::make_reg(MReg::RAX, 8))); // dst = resultado
                /* cae a Ldone */

                O.push_back(MInstr::make_label_def(Ldone));
                break;
            }

            /* GC_HANDLE_FOR_PTR: dst = vrt_gc_handle_for_ptr(proc, ptr).
             * Lookup en ptr_to_handle_ (unordered_map): mas dificil de
             * inline-ar que deref -> sigue via CALL.  Convencion host:
             * proc=arg0, valor=arg1, resultado en RAX. */
            case ir::IrOp::GC_HANDLE_FOR_PTR:
            case ir::IrOp::GC_ALLOC:
            case ir::IrOp::GC_ALLOCP:
            case ir::IrOp::NEWOBJ:
            /* Fase 2: class registry de 1 arg (proc, params_vaddr).
             * Mismo marshalling que gc_handle/newobj.  FINDCLASS/
             * FINDMETHOD/FINDFIELD/DEFCLASS dejan el resultado en dst. */
            case ir::IrOp::FINDCLASS:
            case ir::IrOp::FINDMETHOD:
            case ir::IrOp::FINDFIELD:
            case ir::IrOp::DEFCLASS:
            /* Cluster strings de 1 arg (proc, handle):
             *   STRLEN(handle)      -> i64 code-points
             *   STRGETBYTES(handle) -> i64 byte_len
             *   STRRAW(handle)      -> host_ptr a data[] (is_host_ptr;
             *                          el frontend ya lo marca).
             * Mismo marshalling 1-arg que gc_handle.  STRRAW puede
             * materializar (flatten) -> aloca -> CALL_ABS call-position. */
            case ir::IrOp::STRLEN:
            case ir::IrOp::STRGETBYTES:
            case ir::IrOp::STRRAW:
            case ir::IrOp::RAW_ALLOC: {
                flush_pending();
                /* === Inline slab fast-path (Phase D.7 perf, 2026-06-06) ===
                 * Para RAW_ALLOC con size CONSTANTE que cae en una size
                 * class pequena del slab, inline-amos el pop del free
                 * list (sin CALL al runtime), con fallback CALL @c alloc
                 * cuando el free list de esa clase esta vacio (grow) o
                 * el slab esta deshabilitado (VESTA_NO_SLAB -> free list
                 * siempre null -> siempre slow path, correcto).
                 * Replica EXACTA de RawAllocator::alloc (slab branch):
                 *   node = slab_free_list_[cls];
                 *   if (!node) goto slow;            // grow
                 *   slab_free_list_[cls] = node->next;
                 *   memset(node, 0, SLAB_SIZES[cls]);
                 *   total_bytes_ += SLAB_SIZES[cls];
                 *   return node;
                 * (alloc_count/peak son introspeccion -> se omiten.) */
                if (in.op == ir::IrOp::RAW_ALLOC && vm && ent.raw_alloc != 0 &&
                    in.dst != ir::IR_NO_VALUE && in.operands.size() == 1 &&
                    !jit_no_inline_alloc()) {
                    const ir::IrValueId szv = in.operands[0];
                    if (szv < v_is_const.size() && v_is_const[szv]) {
                        const uint64_t size =
                            static_cast<uint64_t>(v_const[szv]);
                        const size_t cls =
                            gc::RawAllocator::jit_slab_class_for(size);
                        const uint64_t slab_size =
                            (cls == SIZE_MAX)
                                ? 0
                                : gc::RawAllocator::jit_slab_size(cls);
                        /* Solo clases pequenas: zero-init unrolled barato
                         * (<=64B = <=8 stores).  Mayores -> CALL (16+
                         * stores no compensan el ahorro del CALL; medido
                         * 0% en mem_malloc_free, cuyo cuello es el free).
                         * mem_struct (Punto 8B -> clase 16) gana 6.14x. */
                        if (cls != SIZE_MAX && slab_size <= 64 &&
                            slab_size >= 8) {
                            const int32_t ra = vesta_rt::kProcRawAllocOffset;
                            const int32_t fl_off =
                                ra +
                                static_cast<int32_t>(
                                    gc::RawAllocator::
                                        jit_slab_free_list_offset()) +
                                static_cast<int32_t>(cls * 8);
                            const int32_t tb_off =
                                ra +
                                static_cast<int32_t>(
                                    gc::RawAllocator::jit_total_bytes_offset());
                            const MLabelId Lslow = out.new_label();
                            const MLabelId Ldone2 = out.new_label();
                            /* fl = [RBX + fl_off]  (slab_free_list_[cls]) */
                            const ir::IrValueId t_fl = new_tmp();
                            O.push_back(MInstr::make_unary(
                                MOp::MOV, vr(t_fl),
                                MOperand::make_mem(MReg::RBX, fl_off)));
                            /* if (fl == 0) goto slow (free list vacio). */
                            O.push_back(mk_test(t_fl, t_fl));
                            O.push_back(MInstr::make_jcc(MCond::E, Lslow));
                            /* next = [fl] ; slab_free_list_[cls] = next */
                            const ir::IrValueId t_next = new_tmp();
                            O.push_back(MInstr::make_load(vr(t_next), vr(t_fl),
                                                          8, false));
                            O.push_back(MInstr::make_unary(
                                MOp::MOV, MOperand::make_mem(MReg::RBX, fl_off),
                                vr(t_next)));
                            /* zero-init slot: [fl + k] = 0, k=0..slab_size */
                            const ir::IrValueId t_zero = new_tmp();
                            O.push_back(MInstr::make_unary(
                                MOp::MOV, vr(t_zero), MOperand::make_imm32(0)));
                            for (uint64_t k = 0; k < slab_size; k += 8) {
                                if (k == 0) {
                                    O.push_back(MInstr::make_store(
                                        vr(t_fl), vr(t_zero), 8));
                                } else {
                                    const ir::IrValueId t_a = new_tmp();
                                    O.push_back(MInstr::make_unary(
                                        MOp::MOV, vr(t_a), vr(t_fl)));
                                    O.push_back(MInstr::make_binary(
                                        MOp::ADD, vr(t_a), vr(t_a),
                                        MOperand::make_imm32(
                                            static_cast<int32_t>(k))));
                                    O.push_back(MInstr::make_store(
                                        vr(t_a), vr(t_zero), 8));
                                }
                            }
                            /* total_bytes_ += slab_size */
                            const ir::IrValueId t_tb = new_tmp();
                            O.push_back(MInstr::make_unary(
                                MOp::MOV, vr(t_tb),
                                MOperand::make_mem(MReg::RBX, tb_off)));
                            O.push_back(MInstr::make_binary(
                                MOp::ADD, vr(t_tb), vr(t_tb),
                                MOperand::make_imm32(
                                    static_cast<int32_t>(slab_size))));
                            O.push_back(MInstr::make_unary(
                                MOp::MOV, MOperand::make_mem(MReg::RBX, tb_off),
                                vr(t_tb)));
                            /* dst = fl ; jmp done */
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                                           vr(t_fl)));
                            O.push_back(MInstr::make_jmp(Ldone2));
                            /* slow: dst = vrt_raw_alloc(proc, size) */
                            O.push_back(MInstr::make_label_def(Lslow));
#if defined(_WIN32)
                            const MReg za0 = MReg::RCX, za1 = MReg::RDX;
#else
                            const MReg za0 = MReg::RDI, za1 = MReg::RSI;
#endif
                            O.push_back(MInstr::make_unary(
                                MOp::MOV, MOperand::make_reg(za1, 8), vr(szv)));
                            O.push_back(MInstr::make_unary(
                                MOp::MOV, MOperand::make_reg(za0, 8),
                                MOperand::make_reg(MReg::RBX, 8)));
                            O.push_back(MInstr::make_call_abs(
                                out.intern_imm64(ent.raw_alloc)));
                            O.push_back(MInstr::make_unary(
                                MOp::MOV, vr(in.dst),
                                MOperand::make_reg(MReg::RAX, 8)));
                            O.push_back(MInstr::make_label_def(Ldone2));
                            break;
                        }
                    }
                }
                /* GC_ALLOC y GC_ALLOCP bajan ambos a `gcallocp` en el
                 * ir_emitter -> host_ptr al payload (NO un handle).  Por
                 * eso usan gc_alloc_payload, no gc_alloc (que devuelve
                 * handle).  El slot selector mapea GC_ALLOC->gc_alloc
                 * (handle) por error -> store al handle como host_ptr =
                 * crash (era el bug de 64_curry/102/167).  gc_alloc
                 * DISPARA GC (safepoint); los GC roots vivos a traves
                 * del call los spillea el commit 6 (call_position). */
                /* NEWOBJ: vrt_newobj_handle(proc, cls) -> GcHandle.
                 * cls = operands[0] (ClassInfo* nativo, no GC).  El
                 * alloc puede disparar GC; al ser CALL_ABS cuenta como
                 * call-position (interval.cpp) -> los roots vivos a
                 * traves se spillean (commit 6).  Mismo marshalling
                 * 1-arg (proc, valor) que gc_handle/raw_alloc. */
                const uint64_t addr =
                    (in.op == ir::IrOp::GC_HANDLE_FOR_PTR) ? ent.gc_handle
                    : (in.op == ir::IrOp::RAW_ALLOC)       ? ent.raw_alloc
                    : (in.op == ir::IrOp::NEWOBJ)          ? ent.newobj
                    : (in.op == ir::IrOp::FINDCLASS)       ? ent.findclass
                    : (in.op == ir::IrOp::FINDMETHOD)      ? ent.findmethod
                    : (in.op == ir::IrOp::FINDFIELD)       ? ent.findfield
                    : (in.op == ir::IrOp::DEFCLASS)        ? ent.defclass
                    : (in.op == ir::IrOp::STRLEN)          ? ent.str_len
                    : (in.op == ir::IrOp::STRGETBYTES)     ? ent.str_get_bytes
                    : (in.op == ir::IrOp::STRRAW)          ? ent.str_raw
                                                           : ent.gc_allocp;
                if (!vm || addr == 0) {
                    vreg_dbg(fn.name.c_str(), "gc_runtime");
                    return false;
                }
                if (in.operands.size() != 1) return false;
#if defined(_WIN32)
                const MReg ga0 = MReg::RCX, ga1 = MReg::RDX;
#else
                const MReg ga0 = MReg::RDI, ga1 = MReg::RSI;
#endif
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(ga1, 8),
                                       vr(in.operands[0]))); // valor primero
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(ga0, 8),
                                       MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(MInstr::make_call_abs(out.intern_imm64(addr)));
                if (in.dst != ir::IR_NO_VALUE)
                    O.push_back(
                        MInstr::make_unary(MOp::MOV, vr(in.dst),
                                           MOperand::make_reg(MReg::RAX, 8)));
                break;
            }

            /* === Cluster strings (cobertura 2026-06-09) ===
             * STRMAKE(buf, len) [enc=imm] -> GcHandle del StringObject.
             * vrt_str_make(proc, vm_addr, byte_len) lee de vm_mem y
             * auto-detecta ASCII/UTF-8 (el imm enc se ignora, igual que
             * el interp).  3 host args: proc=A0, vm_addr=A1, byte_len=A2.
             * El buf en memoria HOST (is_host_ptr) NO se soporta aun (no
             * hay vrt_str_make_h) -> fallback.  Marshalling robusto via
             * R10/R11 (scratch reservados) para evitar colisiones de
             * arg-reg (misma leccion que DEFFIELD/CALLVIRT).  El alloc
             * puede disparar GC: CALL_ABS = call-position (interval.cpp)
             * -> los roots vivos a traves se spillean (commit 6), y el
             * dst (HANDLE) tambien si vive a traves de otra call. */
            case ir::IrOp::STRMAKE: {
                flush_pending();
                if (!vm || ent.str_make == 0) {
                    vreg_dbg(fn.name.c_str(), "strmake");
                    return false;
                }
                if (in.operands.size() != 2) return false;
                if (in.operands[0] < fn.values.size() &&
                    fn.values[in.operands[0]].is_host_ptr) {
                    /* buf host -> sin runtime entry host todavia. */
                    vreg_dbg(fn.name.c_str(), "strmake_h");
                    return false;
                }
#if defined(_WIN32)
                const MReg sm0 = MReg::RCX, sm1 = MReg::RDX, sm2 = MReg::R8;
#else
                const MReg sm0 = MReg::RDI, sm1 = MReg::RSI, sm2 = MReg::RDX;
#endif
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R10, 8),
                                               vr(in.operands[0]))); // buf
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R11, 8),
                                               vr(in.operands[1]))); // len
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(sm1, 8),
                                       MOperand::make_reg(MReg::R10, 8)));
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(sm2, 8),
                                       MOperand::make_reg(MReg::R11, 8)));
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(sm0, 8),
                                       MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.str_make)));
                if (in.dst != ir::IR_NO_VALUE)
                    O.push_back(
                        MInstr::make_unary(MOp::MOV, vr(in.dst),
                                           MOperand::make_reg(MReg::RAX, 8)));
                break;
            }

            /* STRCAT(a, b) -> handle (ROPE); STRCMP(a, b) -> i64 (-1/0/1).
             * 3 host args: proc=A0, a=A1, b=A2; resultado en RAX.
             * Marshalling robusto via R10/R11 (igual que DEFFIELD): los
             * dos handles a R10/R11 ANTES de moverlos a los arg-regs ->
             * evita colisiones cuando el regalloc asigna un operando a un
             * arg-reg target.  STRCAT aloca (FLAT pequeno o ROPE) -> GC:
             * CALL_ABS = call-position; los handles operandos (si vienen
             * de STRMAKE/STRCAT) estan marcados HANDLE y se spillean. */
            case ir::IrOp::STRCAT:
            case ir::IrOp::STRCMP: {
                flush_pending();
                const uint64_t addr =
                    (in.op == ir::IrOp::STRCAT) ? ent.str_cat : ent.str_cmp;
                if (!vm || addr == 0) {
                    vreg_dbg(fn.name.c_str(), "strcat/strcmp");
                    return false;
                }
                if (in.operands.size() != 2) return false;
#if defined(_WIN32)
                const MReg sc0 = MReg::RCX, sc1 = MReg::RDX, sc2 = MReg::R8;
#else
                const MReg sc0 = MReg::RDI, sc1 = MReg::RSI, sc2 = MReg::RDX;
#endif
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R10, 8),
                                               vr(in.operands[0]))); // a
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R11, 8),
                                               vr(in.operands[1]))); // b
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(sc1, 8),
                                       MOperand::make_reg(MReg::R10, 8)));
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(sc2, 8),
                                       MOperand::make_reg(MReg::R11, 8)));
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(sc0, 8),
                                       MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(MInstr::make_call_abs(out.intern_imm64(addr)));
                if (in.dst != ir::IR_NO_VALUE)
                    O.push_back(
                        MInstr::make_unary(MOp::MOV, vr(in.dst),
                                           MOperand::make_reg(MReg::RAX, 8)));
                break;
            }

            /* === Fase 2 (__module_init -> IR): meta-OOP de 2/3 args. ===
             * DEFFIELD/DEFMETHOD(proc, cls, params) -> 3 host args, sin
             *   dst en IR (el i32/u32 de retorno se descarta).
             * ADDADVICE(proc, target, advice, kind) -> 4 host args, sin dst.
             * SETMETHDBG(proc, params) -> 2 host args; operands[0]=method
             *   se IGNORA (vrt_setmethdbg lee method_ptr del propio params).
             * Marshalling robusto: los args-VALOR se materializan a R10/R11
             * (scratch reservados, NUNCA un vreg) ANTES de moverlos a los
             * arg-regs fijos.  Evita la colision cuando el regalloc asigna
             * un vreg a un arg-reg target (misma leccion que el fix del
             * dispatch CALLVIRT inline).  El CALL_ABS reusa R10 para la
             * direccion, pero R10/R11 ya estan muertos en el call (sus
             * valores se copiaron a los arg-regs). */
            case ir::IrOp::DEFFIELD:
            case ir::IrOp::DEFMETHOD: {
                flush_pending();
                const uint64_t addr = (in.op == ir::IrOp::DEFFIELD)
                                          ? ent.deffield
                                          : ent.defmethod;
                if (!vm || addr == 0) {
                    vreg_dbg(fn.name.c_str(), "deffield/defmethod");
                    return false;
                }
                if (in.operands.size() != 2) return false;
#if defined(_WIN32)
                const MReg da0 = MReg::RCX, da1 = MReg::RDX, da2 = MReg::R8;
#else
                const MReg da0 = MReg::RDI, da1 = MReg::RSI, da2 = MReg::RDX;
#endif
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R10, 8),
                                               vr(in.operands[0]))); // cls
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R11, 8),
                                               vr(in.operands[1]))); // params
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(da1, 8),
                                       MOperand::make_reg(MReg::R10, 8)));
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(da2, 8),
                                       MOperand::make_reg(MReg::R11, 8)));
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(da0, 8),
                                       MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(MInstr::make_call_abs(out.intern_imm64(addr)));
                break;
            }

            case ir::IrOp::ADDADVICE: {
                flush_pending();
                if (!vm || ent.addadvice == 0) {
                    vreg_dbg(fn.name.c_str(), "addadvice");
                    return false;
                }
                if (in.operands.size() != 2) return false;
#if defined(_WIN32)
                const MReg aa0 = MReg::RCX, aa1 = MReg::RDX, aa2 = MReg::R8,
                           aa3 = MReg::R9;
#else
                const MReg aa0 = MReg::RDI, aa1 = MReg::RSI, aa2 = MReg::RDX,
                           aa3 = MReg::RCX;
#endif
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R10, 8),
                                               vr(in.operands[0]))); // target
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R11, 8),
                                               vr(in.operands[1]))); // advice
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(aa1, 8),
                                       MOperand::make_reg(MReg::R10, 8)));
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(aa2, 8),
                                       MOperand::make_reg(MReg::R11, 8)));
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(aa3, 8),
                    MOperand::make_imm32(static_cast<int32_t>(in.imm & 0xFF))));
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(aa0, 8),
                                       MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.addadvice)));
                break;
            }

            case ir::IrOp::SETMETHDBG: {
                flush_pending();
                if (!vm || ent.setmethdbg == 0) {
                    vreg_dbg(fn.name.c_str(), "setmethdbg");
                    return false;
                }
                if (in.operands.size() != 2) return false;
#if defined(_WIN32)
                const MReg sd0 = MReg::RCX, sd1 = MReg::RDX;
#else
                const MReg sd0 = MReg::RDI, sd1 = MReg::RSI;
#endif
                // operands[1]=params (operands[0]=method ignorado).
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(sd1, 8), vr(in.operands[1])));
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(sd0, 8),
                                       MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.setmethdbg)));
                break;
            }

            /* RAW_FREE(ptr) -> void.  free(ptr) del runtime.
             *  - ptr de ALLOCA host-stack  -> NO-OP (lo libera el
             *    epilogue; vrt_raw_free sobre un host-stack ptr crashea).
             *    Es el caso mas inline posible (cero runtime).
             *  - ptr de heap (RAW_ALLOC)   -> CALL vrt_raw_free.  El
             *    free del slab allocator (free lists + size class) es
             *    demasiado complejo para inline-arlo limpiamente (igual
             *    que gc_handle_for_ptr); se queda como CALL. */
            case ir::IrOp::RAW_FREE: {
                flush_pending();
                if (in.operands.size() != 1) return false;
                const ir::IrValueId p = in.operands[0];
                if (p < v_is_host_alloca.size() && v_is_host_alloca[p])
                    break; // host-stack: no-op
                if (!vm || ent.raw_free == 0) {
                    vreg_dbg(fn.name.c_str(), "raw_free(no-vm/no-addr)");
                    return false;
                }
#if defined(_WIN32)
                const MReg fa0 = MReg::RCX, fa1 = MReg::RDX;
#else
                const MReg fa0 = MReg::RDI, fa1 = MReg::RSI;
#endif
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(fa1, 8), vr(p))); // arg1 = ptr
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(fa0, 8),
                    MOperand::make_reg(MReg::RBX, 8))); // arg0 = proc
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.raw_free)));
                break;
            }

            /* CALL intra-modulo a otra funcion Vex (VM_ABI): los args
             * van a proc->registers.regs[1..N] (NO a arg_regs host),
             * proc en RCX/RDI, resultado en regs[0]. */
            case ir::IrOp::CALL: {
                flush_pending();
                /* HOST_LEAF (AOT standalone): convencion del ABI nativo
                 * del host -- args en arg_regs (via ARG), retorno en RAX,
                 * sin ProcessVM* ni proc->registers.  El callee se
                 * referencia por NOMBRE (CALL_SYM): el driver AOT resuelve
                 * su offset en .text tras el layout (relocation), porque al
                 * compilar la funcion aislada la direccion del callee aun
                 * no se conoce.  Cubre tambien la auto-recursion (CALL_SYM
                 * a la propia funcion -> resuelta a su mismo offset). */
                if (abi == AbiKind::HOST_LEAF) {
                    /* Args enteros y flotantes cuentan arg_regs SEPARADOS
                     * (ABI SysV/Win64): emit_host_args reparte por clase. */
                    if (!emit_host_args(in.operands, O)) {
                        vreg_dbg(fn.name.c_str(), "call(host-leaf-args)");
                        return false;
                    }
                    O.push_back(MInstr::make_call_sym(
                        out.intern_reloc_symbol(in.func_name)));
                    if (in.dst != ir::IR_NO_VALUE) {
                        /* Resultado: float -> XMM0 (MOVSD via is_fp_operand);
                         * entero/ptr -> RAX. */
                        const bool dst_f =
                            fp_ok && in.dst < fn.values.size() &&
                            ir_type_is_float(fn.values[in.dst].type);
                        O.push_back(MInstr::make_unary(
                            MOp::MOV,
                            dst_f ? vrt(in.dst) : vr(in.dst),
                            MOperand::make_reg(dst_f ? MReg::XMM0 : MReg::RAX,
                                               8)));
                    }
                    break;
                }
                if (!vm) {
                    vreg_dbg(fn.name.c_str(), "call(no-vm)");
                    return false;
                }
                /* Self-recursion: la propia funcion aun se esta
                 * compilando (g_eager_cache marca EAGER_IN_PROGRESS)
                 * -> resolve_call devuelve 0.  En vez de caer a slots,
                 * emitimos un CALL rel32 a la PROPIA entrada (code+0 =
                 * el prologue, que es el label del bloque 0).  El
                 * prologue recarga los params de proc->registers (que
                 * acabamos de escribir) y monta un frame fresco -> la
                 * recursion corre en JIT de verdad (no trampolin a
                 * interp).  Es codigo non-tail (factorial/fib/quicksort:
                 * el resultado se consume tras la llamada) -> necesita
                 * un frame por nivel, igual que C; la TCO genuina es
                 * IrOp::TAILCALL (caso aparte). */
                const bool is_self = (in.func_name == fn.name);
                uint64_t addr = 0;
                if (!is_self) {
                    if (!resolve_call) {
                        vreg_dbg(fn.name.c_str(), "call(no-resolver)");
                        return false;
                    }
                    addr = resolve_call(in.func_name);
                    if (addr == 0) {
                        vreg_dbg(fn.name.c_str(), "call-unresolved");
                        return false;
                    }
                }
                /* 1. Stores de args a proc->registers.regs[i+1]. */
                for (size_t i = 0; i < in.operands.size(); ++i)
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, vm_reg_mem(static_cast<int>(i) + 1),
                        vr(in.operands[i])));
                /* 2. proc (=RBX) al primer arg host del callee. */
#if defined(_WIN32)
                const MReg proc_reg = MReg::RCX;
#else
                const MReg proc_reg = MReg::RDI;
#endif
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(proc_reg, 8),
                    MOperand::make_reg(MReg::RBX, 8)));
                /* 3. CALL: self -> rel32 a code+0 (label del bloque 0,
                 *    resuelto por el encoder via fixup); externo -> abs. */
                if (is_self)
                    O.push_back(MInstr::make_call_label(blbl[0]));
                else
                    O.push_back(MInstr::make_call_abs(out.intern_imm64(addr)));
                /* 4. Resultado desde regs[0]. */
                if (in.dst != ir::IR_NO_VALUE)
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                                   vm_reg_mem(0)));
                break;
            }

            /* CALLIND (AOT.2.c): llamada INDIRECTA a traves de un puntero
             * a funcion (func_ptr en un vreg).  Base del dispatch virtual
             * nativo (vtable): operands = args, func_ptr = SSA con la
             * direccion del metodo (cargada de la vtable).  HOST_LEAF:
             * args en arg_regs (via ARG) + MOp::CALL(func_ptr) -> el
             * rewrite mueve func_ptr a scratch, hace el parallel-move de
             * los args y emite `call reg`.  Retorno en RAX. */
            case ir::IrOp::CALLIND: {
                flush_pending();
                if (abi == AbiKind::HOST_LEAF) {
                    if (in.func_ptr == ir::IR_NO_VALUE) {
                        vreg_dbg(fn.name.c_str(), "callind(no-fnptr)");
                        return false;
                    }
                    if (!emit_host_args(in.operands, O)) {
                        vreg_dbg(fn.name.c_str(), "callind(host-leaf-args)");
                        return false;
                    }
                    // Devirtualizacion: func_ptr de un LABEL_ADDR conocido ->
                    // CALL DIRECTO (sin indireccion).
                    auto lf = label_fn.find(in.func_ptr);
                    if (lf != label_fn.end()) {
                        O.push_back(MInstr::make_call_sym(
                            out.intern_reloc_symbol(lf->second)));
                    } else {
                        MInstr c;
                        c.op = MOp::CALL;
                        c.src1 = vr(in.func_ptr);
                        O.push_back(c);
                    }
                    if (in.dst != ir::IR_NO_VALUE) {
                        const bool dst_f =
                            fp_ok && in.dst < fn.values.size() &&
                            ir_type_is_float(fn.values[in.dst].type);
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, dst_f ? vrt(in.dst) : vr(in.dst),
                            MOperand::make_reg(dst_f ? MReg::XMM0 : MReg::RAX,
                                               8)));
                    }
                    break;
                }
                vreg_dbg(fn.name.c_str(), "callind(vm-abi)");
                return false;
            }

            /* TAILCALL: tail-call con REUSO de frame (TCO genuina).  El
             * frontend (ir_pass_tailcall) promueve CALL+RET a TAILCALL.
             * Stage de args a proc->registers + pseudo TAILCALL que el
             * rewrite expande a mov A0,rbx + epilogue + jmp al target
             * -> profundidad de pila O(1) (vs el CALL+RET del selector,
             * que NO reusa frame).  self -> jmp rel32 a code+0; cross-fn
             * -> jmp a la addr resuelta.  Sin lectura de resultado: el
             * RET del callee retorna directo al caller original. */
            case ir::IrOp::TAILCALL: {
                flush_pending();
                /* HOST_LEAF (AOT): el ir_pass_tailcall promueve cada
                 * `return f(args)` a TAILCALL.  TCO GENUINA: marshalling
                 * de args a arg_regs (via ARG) + TAILCALL_SYM, que el
                 * rewrite expande a parallel-move + epilogue + JMP al
                 * callee por nombre (relocation rel32).  Reusa el frame
                 * -> profundidad de pila O(1) (igual que el bytecode
                 * tailcall y la TCO del path VM): la optimizacion del IR
                 * se conserva intacta en nativo. */
                if (abi == AbiKind::HOST_LEAF) {
                    if (!emit_host_args(in.operands, O)) {
                        vreg_dbg(fn.name.c_str(), "tailcall(host-leaf-args)");
                        return false;
                    }
                    O.push_back(MInstr::make_tailcall_sym(
                        out.intern_reloc_symbol(in.func_name)));
                    break;
                }
                if (!vm) {
                    vreg_dbg(fn.name.c_str(), "tailcall(no-vm)");
                    return false;
                }
                const bool is_self = (in.func_name == fn.name);
                uint64_t addr = 0;
                if (!is_self) {
                    if (!resolve_call) {
                        vreg_dbg(fn.name.c_str(), "tailcall(no-resolver)");
                        return false;
                    }
                    addr = resolve_call(in.func_name);
                    if (addr == 0) {
                        vreg_dbg(fn.name.c_str(), "tailcall-unresolved");
                        return false;
                    }
                }
                /* 1. Stores de args a proc->registers.regs[i+1] (igual
                 *    que CALL; los args son vregs del frame actual). */
                for (size_t i = 0; i < in.operands.size(); ++i)
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, vm_reg_mem(static_cast<int>(i) + 1),
                        vr(in.operands[i])));
                /* 2. Pseudo TAILCALL (el rewrite hace proc->A0 +
                 *    epilogue + jmp).  Termina el bloque (terminador). */
                if (is_self)
                    O.push_back(MInstr::make_tailcall_label(blbl[0]));
                else
                    O.push_back(
                        MInstr::make_tailcall_abs(out.intern_imm64(addr)));
                break;
            }

            /* CALLVIRT: dispatch dinamico via vrt_callvirt(proc, obj,
             * vtbl_idx).  Los args reales van a proc->registers; proc
             * /obj/vtbl_idx van a arg_regs host; el resultado queda en
             * regs[0]. */
            case ir::IrOp::CALLVIRT: {
                flush_pending();
                if (!vm || ent.callvirt == 0) {
                    vreg_dbg(fn.name.c_str(), "callvirt(no-vm/no-addr)");
                    return false;
                }
                if (in.operands.empty()) return false;
                const ir::IrValueId obj = in.operands[0];
                const uint32_t vtbl_idx = static_cast<uint32_t>(in.imm);
                /* 1. Stores de args reales a proc->registers.regs[i+1]
                 *    (operands[0]=obj=this -> regs[1], args -> regs[2..]). */
                for (size_t i = 0; i < in.operands.size(); ++i)
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, vm_reg_mem(static_cast<int>(i) + 1),
                        vr(in.operands[i])));
                /* 2. arg regs host (arg0=proc, arg1=obj, arg2=vtbl_idx). */
#if defined(_WIN32)
                const MReg pr_reg = MReg::RCX, obj_reg = MReg::RDX,
                           idx_reg = MReg::R8;
#else
                const MReg pr_reg = MReg::RDI, obj_reg = MReg::RSI,
                           idx_reg = MReg::RDX;
#endif
                auto emit_callvirt_slow = [&]() {
                    /* vrt_callvirt(proc, obj, vtbl_idx).  obj PRIMERO. */
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(obj_reg, 8), vr(obj)));
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(pr_reg, 8),
                        MOperand::make_reg(MReg::RBX, 8)));
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(idx_reg, 8),
                        MOperand::make_imm32(static_cast<int32_t>(vtbl_idx))));
                    O.push_back(
                        MInstr::make_call_abs(out.intern_imm64(ent.callvirt)));
                };
                if (!jit_no_inline_callvirt()) {
                    /* 3a. INLINE DISPATCH: class_ptr -> vtable -> method
                     *     -> jit_code y call directo (indirecto) si el
                     *     metodo esta compilado y sin advices.  Fallback
                     *     a vrt_callvirt en cualquier otro caso. */
                    const MLabelId Lfb = out.new_label();
                    const MLabelId Ldone = out.new_label();
                    auto load_field = [&](ir::IrValueId base,
                                          int32_t off) -> ir::IrValueId {
                        const ir::IrValueId d = new_tmp();
                        if (off == 0) {
                            O.push_back(
                                MInstr::make_load(vr(d), vr(base), 8, false));
                        } else {
                            const ir::IrValueId addr = new_tmp();
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(addr),
                                                           vr(base)));
                            O.push_back(MInstr::make_binary(
                                MOp::ADD, vr(addr), vr(addr),
                                MOperand::make_imm32(off)));
                            O.push_back(
                                MInstr::make_load(vr(d), vr(addr), 8, false));
                        }
                        return d;
                    };
                    /* cls = [obj]  (class_ptr offset 0). */
                    const ir::IrValueId cls = load_field(obj, 0);
                    O.push_back(mk_test(cls, cls));
                    O.push_back(MInstr::make_jcc(MCond::E, Lfb));
                    /* vtbl = [cls + VTABLE_OFFSET]. */
                    const ir::IrValueId vtbl =
                        load_field(cls, VESTA_CLASSINFO_VTABLE_OFFSET);
                    O.push_back(mk_test(vtbl, vtbl));
                    O.push_back(MInstr::make_jcc(MCond::E, Lfb));
                    /* method = [vtbl + vtbl_idx*8]. */
                    const ir::IrValueId method =
                        load_field(vtbl, static_cast<int32_t>(vtbl_idx * 8u));
                    O.push_back(mk_test(method, method));
                    O.push_back(MInstr::make_jcc(MCond::E, Lfb));
                    /* advice = [method + ADVICE_CHAIN_OFFSET]; si != 0
                     * (tiene aspectos AOP) -> slow path. */
                    const ir::IrValueId adv = load_field(
                        method, VESTA_METHODINFO_ADVICE_CHAIN_OFFSET);
                    O.push_back(mk_test(adv, adv));
                    O.push_back(MInstr::make_jcc(MCond::NE, Lfb));
                    /* code = [method + JIT_CODE_OFFSET]; si 0 -> slow. */
                    const ir::IrValueId code =
                        load_field(method, VESTA_METHODINFO_JIT_CODE_OFFSET);
                    O.push_back(mk_test(code, code));
                    O.push_back(MInstr::make_jcc(MCond::E, Lfb));
                    /* FAST: proc en arg0; call directo (indirecto) a
                     * code (los args ya estan en proc->registers).
                     *
                     * BUG FIX (loop+callvirt+objeto-GC): el regalloc
                     * puede asignar `code` a pr_reg (RCX en Win64 /
                     * RDI en SysV = arg0).  El `mov pr_reg, rbx`
                     * (proc) lo machacaria ANTES del call -> se
                     * llamaria a `proc` (ProcessVM*) en vez de a code
                     * -> SIGSEGV.  El regalloc NO modela el `mov
                     * pr_reg, rbx` (write a fisico) como interferencia
                     * con el vreg `code`.  Solucion: mover code a R10
                     * (SCRATCH reservado, NUNCA asignable a un vreg ->
                     * sin colision posible) antes de escribir pr_reg,
                     * y hacer el call indirecto via R10.  El `mov
                     * pr_reg, rbx` (reg-reg directo) no toca R10. */
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(MReg::R10, 8), vr(code)));
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(pr_reg, 8),
                        MOperand::make_reg(MReg::RBX, 8)));
                    {
                        MInstr ic;
                        ic.op = MOp::CALL;
                        ic.src1 = MOperand::make_reg(MReg::R10, 8);
                        O.push_back(ic);
                    }
                    O.push_back(MInstr::make_jmp(Ldone));
                    /* FALLBACK. */
                    O.push_back(MInstr::make_label_def(Lfb));
                    emit_callvirt_slow();
                    O.push_back(MInstr::make_label_def(Ldone));
                } else {
                    emit_callvirt_slow();
                }
                /* 4. Resultado en regs[0]. */
                if (in.dst != ir::IR_NO_VALUE)
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                                   vm_reg_mem(0)));
                break;
            }

            /* CALLCLOSURE: %dst = vrt_callclosure(proc, fn_addr, env).
             * func_ptr = SSA con fn_addr (helper __lambda_N o jit_code);
             * operands[0] = env_ptr (0 si sin captures); operands[1..] =
             * args declarados.  El runtime entry coloca env en R14 y
             * dispatcha (VM bytecode via mini-interp si fn_addr<4GB, o
             * jit_code via enter_jit si >4GB).  Los args van a
             * proc->registers.regs[1..N] + regs[15]=nargs (convencion de
             * los helpers __lambda_N, igual que CALL/CALLVIRT).  El
             * CALL_ABS es call-position -> GC roots vivos se spillean;
             * env (si is_gc_object) tambien. */
            case ir::IrOp::CALLCLOSURE: {
                flush_pending();
                /* HOST_LEAF (AOT bare): una closure es {fn_addr, env}.
                 * func_ptr ya es el fn_addr cargado de closure[0];
                 * operands[0] = env, operands[1..] = args.  Las lambdas SIN
                 * capturas (las unicas compilables en bare: las capturantes
                 * usan READ_VM_REG, que aot_analyze rechaza) no leen env ->
                 * emitimos una llamada indirecta plana al fn_addr con los
                 * args en arg_regs (= puntero de funcion C; si el optimizador
                 * conoce la lambda, devirtualiza a CALL directo). */
                if (abi == AbiKind::HOST_LEAF) {
                    if (in.func_ptr == ir::IR_NO_VALUE) {
                        vreg_dbg(fn.name.c_str(), "callclosure(no-fnptr)");
                        return false;
                    }
                    // ABI bare: helper = R __lambda(args..., void* env).
                    // operands[0] = env, operands[1..] = args.  Pasamos los
                    // args y luego el env como ULTIMO arg.  Lambdas sin
                    // capturas: env es 0 y el helper no tiene param env -> el
                    // arg extra sobra inofensivamente (caller-clean ABI).
                    std::vector<ir::IrValueId> cargs;
                    if (in.operands.size() > 1)
                        cargs.assign(in.operands.begin() + 1,
                                     in.operands.end());
                    if (!in.operands.empty())
                        cargs.push_back(in.operands[0]); // env al final
                    if (!emit_host_args(cargs, O)) {
                        vreg_dbg(fn.name.c_str(), "callclosure(host-leaf-args)");
                        return false;
                    }
                    // Devirtualizacion: si el fn_addr proviene de un LABEL_ADDR
                    // conocido (lambda conocida estaticamente) -> CALL DIRECTO
                    // (sin indireccion, mejor que un puntero de funcion C).
                    auto lf = label_fn.find(in.func_ptr);
                    if (lf != label_fn.end()) {
                        O.push_back(MInstr::make_call_sym(
                            out.intern_reloc_symbol(lf->second)));
                    } else {
                        MInstr cc;
                        cc.op = MOp::CALL;
                        cc.src1 = vr(in.func_ptr);
                        O.push_back(cc);
                    }
                    if (in.dst != ir::IR_NO_VALUE) {
                        const bool dst_f =
                            fp_ok && in.dst < fn.values.size() &&
                            ir_type_is_float(fn.values[in.dst].type);
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, dst_f ? vrt(in.dst) : vr(in.dst),
                            MOperand::make_reg(dst_f ? MReg::XMM0 : MReg::RAX,
                                               8)));
                    }
                    break;
                }
                if (!vm || ent.callclosure == 0) {
                    vreg_dbg(fn.name.c_str(), "callclosure(no-vm/no-addr)");
                    return false;
                }
                if (in.func_ptr == ir::IR_NO_VALUE || in.operands.empty()) {
                    vreg_dbg(fn.name.c_str(), "callclosure(shape)");
                    return false;
                }
                const size_t nargs = in.operands.size() - 1;
                if (nargs > 12) {
                    vreg_dbg(fn.name.c_str(), "callclosure-args");
                    return false;
                }
                /* 1. Stores de args (operands[1..]) a regs[1..N]. */
                for (size_t i = 0; i < nargs; ++i)
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, vm_reg_mem(static_cast<int>(i) + 1),
                        vr(in.operands[i + 1])));
                /* 2. regs[15] = nargs. */
                O.push_back(MInstr::make_unary(
                    MOp::MOV, vm_reg_mem(15),
                    MOperand::make_imm32(static_cast<int32_t>(nargs))));
                /* 3. vrt_callclosure(proc, fn_addr, env).  fn_addr/env a
                 *    R10/R11 (scratch) antes de los arg-regs -> sin
                 *    colision (idem STRCAT/DEFFIELD). */
#if defined(_WIN32)
                const MReg cc0 = MReg::RCX, cc1 = MReg::RDX, cc2 = MReg::R8;
#else
                const MReg cc0 = MReg::RDI, cc1 = MReg::RSI, cc2 = MReg::RDX;
#endif
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R10, 8),
                                               vr(in.func_ptr))); // fn_addr
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R11, 8),
                                               vr(in.operands[0]))); // env
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(cc1, 8),
                                       MOperand::make_reg(MReg::R10, 8)));
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(cc2, 8),
                                       MOperand::make_reg(MReg::R11, 8)));
                O.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(cc0, 8),
                                       MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.callclosure)));
                /* 4. Resultado (RAX). */
                if (in.dst != ir::IR_NO_VALUE)
                    O.push_back(
                        MInstr::make_unary(MOp::MOV, vr(in.dst),
                                           MOperand::make_reg(MReg::RAX, 8)));
                break;
            }

            /* CALLN: FFI a funcion nativa.  CALL DIRECTO a la direccion
             * resuelta en compile-time (resolve_native) -- NO via el
             * dispatcher runtime vrt_calln (mas lento).  Los args van a
             * arg_regs host (convencion C) via ARG; CALL_ABS hace el
             * parallel-move + CALL.  Resultado en RAX. */
            case ir::IrOp::CALLN: {
                flush_pending();
                /* Inline de math intrinsics (principio: evitar el
                 * runtime).  vmath_abs -> sar/xor/sub (~4 instr) en vez
                 * de un CALL a vesta_math. */
                if (in.func_name.find("vmath_abs") != std::string::npos &&
                    in.dst != ir::IR_NO_VALUE && in.operands.size() == 1) {
                    const ir::IrValueId tmp = new_tmp();
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(tmp),
                                                   vr(in.operands[0])));
                    O.push_back(MInstr::make_binary(MOp::SAR, vr(tmp), vr(tmp),
                                                    MOperand::make_imm32(63)));
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                                   vr(in.operands[0])));
                    O.push_back(MInstr::make_binary(MOp::XOR, vr(in.dst),
                                                    vr(in.dst), vr(tmp)));
                    O.push_back(MInstr::make_binary(MOp::SUB, vr(in.dst),
                                                    vr(in.dst), vr(tmp)));
                    break;
                }
                /* vmath_ilog2(n) = 63 - lzcnt(n).  Paridad EXACTA con el
                 * slot selector, incluido el edge n==0 (lzcnt(0)=64 ->
                 * -1), que diverge del builtin C (0) pero mantiene
                 * slots==vregs (el frontend no genera ilog2(0)). */
                if (in.func_name.find("vmath_ilog2") != std::string::npos &&
                    in.dst != ir::IR_NO_VALUE && in.operands.size() == 1) {
                    O.push_back(MInstr::make_unary(MOp::LZCNT, vr(in.dst),
                                                   vr(in.operands[0])));
                    O.push_back(
                        MInstr::make_unary(MOp::NEG, vr(in.dst), vr(in.dst)));
                    O.push_back(MInstr::make_binary(MOp::ADD, vr(in.dst),
                                                    vr(in.dst),
                                                    MOperand::make_imm32(63)));
                    break;
                }
                /* vmath_rotl/rotr con count CONSTANTE -> ROL/ROR dst, imm.
                 * Count variable necesita CL -> fallback al CALL. */
                if ((in.func_name.find("vmath_rotl") != std::string::npos ||
                     in.func_name.find("vmath_rotr") != std::string::npos) &&
                    in.dst != ir::IR_NO_VALUE && in.operands.size() == 2) {
                    const ir::IrValueId cnt = in.operands[1];
                    if (cnt < v_is_const.size() && v_is_const[cnt]) {
                        const int32_t amt =
                            static_cast<int32_t>(v_const[cnt] & 63);
                        const MOp rop = (in.func_name.find("vmath_rotl") !=
                                         std::string::npos)
                                            ? MOp::ROL
                                            : MOp::ROR;
                        O.push_back(MInstr::make_binary(
                            rop, vr(in.dst), vr(in.operands[0]),
                            MOperand::make_imm32(amt)));
                        break;
                    }
                    /* count variable -> cae al CALL de abajo. */
                }
                /* vmath_min/max/minu/maxu -> CMP + CMOVcc (dst=a; si
                 * cc(a,b) dst=b).  Paridad EXACTA con el slot: minu->A,
                 * maxu->B, min->G (signed), max->L (signed). */
                if ((in.func_name.find("vmath_min") != std::string::npos ||
                     in.func_name.find("vmath_max") != std::string::npos) &&
                    in.dst != ir::IR_NO_VALUE && in.operands.size() == 2) {
                    const ir::IrValueId a = in.operands[0];
                    const ir::IrValueId b = in.operands[1];
                    MCond cc;
                    if (in.func_name.find("vmath_minu") != std::string::npos)
                        cc = MCond::A;
                    else if (in.func_name.find("vmath_maxu") !=
                             std::string::npos)
                        cc = MCond::B;
                    else if (in.func_name.find("vmath_min") !=
                             std::string::npos)
                        cc = MCond::G;
                    else
                        cc = MCond::L; // vmath_max
                    O.push_back(
                        MInstr::make_unary(MOp::MOV, vr(in.dst), vr(a)));
                    O.push_back(mk_cmp(a, b));
                    MInstr cm;
                    cm.op = MOp::CMOVCC;
                    cm.variant = static_cast<uint8_t>(cc);
                    cm.dst = vr(in.dst);
                    cm.src1 = vr(b);
                    O.push_back(cm);
                    break;
                }
                /* HOST_LEAF (AOT): un CALLN a un extern (FFI nativo, p.ej.
                 * @AllocatorOverride que envuelve kmalloc) no tiene
                 * direccion en compile time -> se referencia por NOMBRE
                 * (CALL_SYM) y lo resuelve el linker.  El func_name viene
                 * como "lib:simbolo"; el linker solo necesita el simbolo
                 * (la lib la aporta el enlace).  Los intrinsics vmath_*
                 * ya se han inline-ado arriba sin tocar esta rama. */
                if (abi == AbiKind::HOST_LEAF) {
                    const size_t nargs = in.operands.size();
                    std::string sym = in.func_name;
                    const size_t colon = sym.rfind(':');
                    if (colon != std::string::npos) sym = sym.substr(colon + 1);
                    if (mode32) {
                        /* x86-32: un CALLN a un extern (libc/FFI) es CDECL
                         * (args en la PILA, retorno en eax, caller limpia).
                         * Las funciones internas usan regparm, pero el ABI
                         * de libc es cdecl -> aqui marshalamos a pila.
                         * Staging por eax (caller-saved, lo clobbea el call)
                         * -> evita PUSH de un vreg. */
                        for (size_t a = nargs; a-- > 0;) {
                            O.push_back(MInstr::make_unary(
                                MOp::MOV, MOperand::make_reg(MReg::RAX, 4),
                                vr(in.operands[a])));
                            MInstr p;
                            p.op = MOp::PUSH;
                            p.src1 = MOperand::make_reg(MReg::RAX, 4);
                            O.push_back(p);
                        }
                        O.push_back(MInstr::make_call_sym(
                            out.intern_reloc_symbol(sym)));
                        if (nargs)
                            O.push_back(MInstr::make_binary(
                                MOp::ADD, MOperand::make_reg(MReg::RSP, 4),
                                MOperand::make_reg(MReg::RSP, 4),
                                MOperand::make_imm32(
                                    static_cast<int32_t>(nargs * 4))));
                        if (in.dst != ir::IR_NO_VALUE)
                            O.push_back(MInstr::make_unary(
                                MOp::MOV, vr(in.dst),
                                MOperand::make_reg(MReg::RAX, 4)));
                        break;
                    }
                    if (nargs > host_leaf_nmax) {
                        vreg_dbg(fn.name.c_str(), "calln(host-leaf-args)");
                        return false;
                    }
                    for (size_t a = 0; a < nargs; ++a)
                        O.push_back(MInstr::make_arg(static_cast<uint8_t>(a),
                                                     vr(in.operands[a])));
                    O.push_back(
                        MInstr::make_call_sym(out.intern_reloc_symbol(sym)));
                    if (in.dst != ir::IR_NO_VALUE)
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, vr(in.dst),
                            MOperand::make_reg(MReg::RAX, 8)));
                    break;
                }
                if (!resolve_native) {
                    vreg_dbg(fn.name.c_str(), "calln(no-resolver)");
                    return false;
                }
                const uint64_t fn_addr = resolve_native(in.func_name);
                if (fn_addr == 0) {
                    vreg_dbg(fn.name.c_str(), "calln-unresolved");
                    return false;
                }
                const size_t nargs = in.operands.size();
#if defined(_WIN32)
                const size_t nmax = 4;
#else
                const size_t nmax = 6;
#endif
                if (nargs > nmax) { // stack args no soportados
                    vreg_dbg(fn.name.c_str(), "calln-args");
                    return false;
                }
                for (size_t a = 0; a < nargs; ++a)
                    O.push_back(MInstr::make_arg(static_cast<uint8_t>(a),
                                                 vr(in.operands[a])));
                O.push_back(MInstr::make_call_abs(out.intern_imm64(fn_addr)));
                if (in.dst != ir::IR_NO_VALUE)
                    O.push_back(
                        MInstr::make_unary(MOp::MOV, vr(in.dst),
                                           MOperand::make_reg(MReg::RAX, 8)));
                break;
            }

            /* SMARTPTR_FREE: cleanup deterministico de unique<T> con
             * deleter custom (el deleter `free` por defecto baja a
             * RAW_FREE, ya soportado).  3 variantes (imm):
             *   1 = EXTERN_CALLN:  deleter FFI nativo.  if(ptr) calln(ptr).
             *   2 = VESTA_CALLVM:  deleter Vesta estatico.  if(ptr)
             * callvm(ptr). 0 = SRET_DISPATCH: deleter dinamico del slot+8 (caso
             * SRET factory, raro) -> FALLBACK por ahora (call dinamico + free,
             * mas delicado; sesion dedicada). Estructura: null-check ptr; si
             * !=0, invoca el deleter con ptr como unico arg.  is_call_site ->
             * el CALL_ABS es call-position (GC roots vivos a traves se
             * spillean).  El ptr es RAW_ALLOC (no GC, no se marca root) -> el
             * deleter lo libera. */
            case ir::IrOp::SMARTPTR_FREE: {
                flush_pending();
                if (in.operands.empty()) break; // idem ir_emitter (no-op)
                const ir::IrValueId ptr = in.operands[0];
                uint64_t addr = 0;
                if (in.imm == 1) {
                    if (!resolve_native) {
                        vreg_dbg(fn.name.c_str(), "smartptr_free(no-nat)");
                        return false;
                    }
                    addr = resolve_native(in.func_name);
                } else if (in.imm == 2) {
                    if (!vm || !resolve_call) {
                        vreg_dbg(fn.name.c_str(), "smartptr_free(no-call)");
                        return false;
                    }
                    addr = resolve_call(in.func_name);
                } else {
                    /* kind 0 (SRET dynamic) -> fallback (call dinamico). */
                    vreg_dbg(fn.name.c_str(), "smartptr_free(kind0)");
                    return false;
                }
                if (addr == 0) {
                    vreg_dbg(fn.name.c_str(), "smartptr_free-unresolved");
                    return false;
                }
                const MLabelId L_done = out.new_label();
                O.push_back(mk_test(ptr, ptr));
                O.push_back(MInstr::make_jcc(MCond::E, L_done));
                if (in.imm == 1) {
                    /* EXTERN_CALLN: deleter(ptr) -- arg0 host directo. */
                    O.push_back(MInstr::make_arg(0, vr(ptr)));
                    O.push_back(MInstr::make_call_abs(out.intern_imm64(addr)));
                } else { /* imm == 2: VESTA_CALLVM (convencion VM) */
                    O.push_back(MInstr::make_unary(MOp::MOV, vm_reg_mem(1),
                                                   vr(ptr))); // ptr -> regs[1]
#if defined(_WIN32)
                    const MReg pr = MReg::RCX;
#else
                    const MReg pr = MReg::RDI;
#endif
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(pr, 8),
                        MOperand::make_reg(MReg::RBX, 8))); // proc -> arg0
                    O.push_back(MInstr::make_call_abs(out.intern_imm64(addr)));
                }
                O.push_back(MInstr::make_label_def(L_done));
                break;
            }

            case ir::IrOp::STR_LIT_ADDR: {
                /* %dst = direccion VM del literal de string indexado
                 * por @c in.imm en el bloque "code.s_<imm>" del .velb.
                 * Equivalente al `mov rDst, @Absolute("code.s_<imm>")`
                 * que emite el frontend; lo resolvemos en compile-time
                 * via @c resolve_symbol (Phase D.3-H, igual que el
                 * selector de slots).
                 *
                 * IMPORTANTE: el resultado es un VM-addr (offset a
                 * static_data en proc->vm_mem), NO un host_ptr.  El
                 * value del IR queda con is_host_ptr=false, asi que un
                 * LOAD/STORE posterior sobre el cae al fallback (el
                 * vreg solo soporta LOAD/STORE host).  No marcamos
                 * host-ness aqui: solo emitimos el inmediato. */
                flush_pending();
                if (in.dst == ir::IR_NO_VALUE) return false;
                /* AOT (HOST_LEAF): el literal vive en .rodata, NO en vm_mem.
                 * Emitimos una referencia por NOMBRE ("rodata.<imm>") que el
                 * driver resuelve contra el offset de la entry en el pool de
                 * static_data, y el writer parchea tras el layout.  PIC
                 * (default) -> lea [rip+disp32] (DATA_REL32, position-
                 * independent); --no-pie -> mov reg,imm64 (ABS64).  El
                 * resultado es un host_ptr real (los LOAD posteriores en
                 * HOST_LEAF ya son host directos). */
                if (abi == AbiKind::HOST_LEAF) {
                    const uint32_t sidx = out.intern_reloc_symbol(
                        "rodata." + std::to_string(in.imm));
                    if (pic)
                        O.push_back(MInstr::make_lea_rip_sym(vr(in.dst), sidx));
                    else
                        O.push_back(MInstr::make_mov_sym(vr(in.dst), sidx));
                    break;
                }
                uint64_t addr = 0;
                if (resolve_symbol)
                    addr = resolve_symbol("code.s_" + std::to_string(in.imm));
                if (addr == 0) {
                    vreg_dbg(fn.name.c_str(), "str_lit_addr(no-symbol)");
                    return false; // sin resolver -> fallback a slots
                }
                const uint32_t idx = out.intern_imm64(addr);
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               MOperand::make_imm64_idx(idx)));
                break;
            }

            case ir::IrOp::LABEL_ADDR: {
                /* %dst = direccion VM del label `code.<func_name>`
                 * resuelta por el linker.  Equivalente al
                 * `mov rDst, @Absolute("code.<func_name>")` que emite
                 * el frontend (B.1 as_native_callback, paso de fn por
                 * valor, trampolines, registro de handlers).  Lo
                 * resolvemos via @c resolve_symbol igual que el
                 * selector de slots (Phase D.3-H).
                 *
                 * Es un PC virtual (code addr), NO un host_ptr: mismo
                 * tratamiento que STR_LIT_ADDR -- solo emitimos el
                 * inmediato, sin marcar host-ness. */
                flush_pending();
                if (in.dst == ir::IR_NO_VALUE || in.func_name.empty()) {
                    vreg_dbg(fn.name.c_str(), "label_addr(no-func_name)");
                    return false;
                }
                /* AOT (HOST_LEAF): la direccion de la funcion no se conoce
                 * en codegen (cada funcion se compila aislada).  Emitimos una
                 * referencia por NOMBRE "fnsym:<func_name>" que el driver
                 * resuelve contra el offset de la funcion en .text tras el
                 * layout (igual que rodata.<N> para datos).  PIC -> lea
                 * [rip+disp32] (DATA_REL32); --no-pie -> mov reg,imm64
                 * (ABS64).  Es un host-addr real (codigo nativo): un puntero
                 * de funcion valido para CALLIND.  Base del despacho de
                 * helpers multi-versionados por CPU (Inc 2). */
                if (abi == AbiKind::HOST_LEAF) {
                    const uint32_t sidx =
                        out.intern_reloc_symbol("fnsym:" + in.func_name);
                    if (pic)
                        O.push_back(MInstr::make_lea_rip_sym(vr(in.dst), sidx));
                    else
                        O.push_back(MInstr::make_mov_sym(vr(in.dst), sidx));
                    // Recordar que dst es la direccion de esta funcion conocida
                    // -> permite devirtualizar un CALL indirecto sobre ella.
                    label_fn[in.dst] = in.func_name;
                    break;
                }
                /* VM/JIT: resolvemos via resolve_symbol (Phase D.3-H). */
                uint64_t addr = 0;
                if (resolve_symbol)
                    addr = resolve_symbol("code." + in.func_name);
                if (addr == 0) {
                    vreg_dbg(fn.name.c_str(), "label_addr(no-symbol)");
                    return false; // sin resolver -> fallback a slots
                }
                const uint32_t idx = out.intern_imm64(addr);
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               MOperand::make_imm64_idx(idx)));
                break;
            }

            /* SECTION_REF (AOT dev OS): simbolo de seccion start/end/size.
             * func_name = nombre de seccion, imm = kind (0/1/2).  En
             * HOST_LEAF emitimos una ref por NOMBRE "secsym:<k>:<name>"
             * que el driver mapea a (ADDR/END/SIZE de la seccion):
             *   START/END = direccion -> lea[rip] (pic) o mov imm64 (no-pie);
             *   SIZE = constante -> SIEMPRE mov imm64 (un tamano no es una
             *   direccion, no aplica RIP-relativo).
             * En VM_ABI (JIT) no hay secciones nativas -> 0. */
            case ir::IrOp::SECTION_REF: {
                flush_pending();
                if (in.dst == ir::IR_NO_VALUE) return false;
                if (abi != AbiKind::HOST_LEAF) {
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                                   MOperand::make_imm32(0)));
                    break;
                }
                const char kc = (in.imm == 0) ? 's' : (in.imm == 1) ? 'e' : 'z';
                const uint32_t sidx = out.intern_reloc_symbol(
                    std::string("secsym:") + kc + ":" + in.func_name);
                const bool is_size = (in.imm == 2);
                if (pic && !is_size)
                    O.push_back(MInstr::make_lea_rip_sym(vr(in.dst), sidx));
                else
                    O.push_back(MInstr::make_mov_sym(vr(in.dst), sidx));
                break;
            }

            case ir::IrOp::GETPROC: {
                /* %dst = ProcessVM* del proceso actual.  En VM_ABI el
                 * proc esta en RBX (reservado, preservado por el
                 * prologue); GETPROC = `mov dst, rbx`, igual que el
                 * selector de slots.  Fuera de VM_ABI no hay un proc
                 * accesible -> fallback.  El resultado es un host_ptr
                 * nativo (no objeto GC): vreg_is_gc[dst]=0 y un LOAD
                 * posterior sobre el (proc->campo) es host (soportado). */
                flush_pending();
                if (!vm || in.dst == ir::IR_NO_VALUE) {
                    vreg_dbg(fn.name.c_str(), "getproc(no-vm)");
                    return false;
                }
                O.push_back(MInstr::make_unary(
                    MOp::MOV, vr(in.dst), MOperand::make_reg(MReg::RBX, 8)));
                break;
            }

            default:
                vreg_dbg(fn.name.c_str(), ir::ir_op_name(in.op));
                return false; // op fuera del subset -> fallback
            }
        }
        flush_pending(); // por si el bloque termina sin terminador explicito
        lm_flush();       // solo-LSP: estampar la ultima op del bloque
        out.blocks.push_back(std::move(mb));
    }
    return true;
}

} // namespace jit
