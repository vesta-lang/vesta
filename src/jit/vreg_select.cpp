/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
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

#include <unordered_map>
#include <unordered_set>

#include "jit/auto_jit.h" // FN.3: lookup_jit_code_at_pc (LABEL_ADDR -> nativo)

#include "ir/ssa_ir.h"
#include "vesta_rt/abi.h"
#include "jit/target_reginfo.h" // Phase AOT.3 2b: arg_regs del ABI host (HOST_LEAF)
#include "jit/vec_isa.h"        // ancho SIMD (SSE2/AVX2/AVX512) del VEC_BINOP
#include "gc/raw_allocator.h" // Phase D.7 perf: inline slab fast-path
#include "vx/asm/asm_backend.h"  // Phase AS inc.5: ensamblar inline-asm -> bytes
#include "vx/asm/asm_effects.h"  // Phase AS inc.5: asm_canonical_reg
#include "vx/asm/asm_phys_reg.h" // sustitucion $N -> reg fisico
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
#include <string>  // std::string (UCRT64 no lo incluye transitivo)
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
            // SWITCH_DENSE: remapear la tabla de bloques destino.
            for (uint32_t &t : in.jump_targets) t = rm(t);
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
 *        bits (de @c vx::asm_canonical_reg) al id de @c MReg GP (0..15),
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
                 bool mode32, FloatIsa fisa, bool emit_line_map,
                 const VregCallbackOpts &cb) {
    /* Callback-ABI (jubilacion de slots): un callback nativo se compila en
     * VM_ABI pero con un prologo que carga proc y marshalea los args nativos a
     * proc->registers antes de que el prologo normal (mas abajo) los relea.  El
     * cuerpo y el RET consultan @c cb_entry.  Solo aplica en AbiKind::VM. */
    const bool cb_entry = cb.callback_entry && abi == AbiKind::VM;
    /* Callback save-set: SIEMPRE para callbacks.  El marshalling del prologo
     * escribe proc->registers[1..N] (+ argc) y el RET escribe regs[0]; si el
     * callback se invoca desde un contexto donde proc->registers es el banco de
     * registros VIVO del caller (el INTERPRETE, o una re-entrada), eso lo
     * corromperia.  No podemos saber el contexto del caller en compile-time, asi
     * que salvamos/restauramos regs[0..15] incondicionalmente (CB_SAVE_REGS en
     * el prologo, CB_RESTORE_REGS en cada RET; el rewrite reserva 128B).  En
     * codigo JIT-eado el caller no usa proc->registers a traves del call, pero
     * el coste (32 mem-ops) es la garantia de correctness pedida. */
    const bool cb_save_set = cb_entry;
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
    /* El split se aplica en AMBOS paths (AOT HOST_LEAF y JIT VM_ABI).  Un
     * `if(c){n=v}` con PHI en el merge es el bloqueante real: sin el split
     * estas funciones caian al selector de slots (legacy, con los bugs
     * B-JIT-1).  El "bug latente" que antes desaconsejaba el split en VM_ABI
     * (vreg=valor-basura en 33_optional_result_builtin) NO era del split ni
     * del paso de punteros a ALLOCA entre funciones vreg: era el MIX
     * vreg-caller (frame host) + callee-en-SLOTS (direccionamiento VM) para
     * el mismo ALLOCA.  Al hacer que UNWRAP compile por vreg en VM_ABI
     * (vrt_unwrap_throw), el callee deja de caer a slots -> caller y callee
     * coinciden en direccionamiento -> correcto.  Validado: diff_harness
     * vreg==interp en todo el corpus.  Esto retira slots de ~27 funciones
     * con control de flujo + PHI (el mayor causante de fallback). */
    if (!no_split && has_critical_edge_to_phi(fn_in)) {
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
    /* Callback save-set: propagar tras el reset de out (arriba lo borra un
     * MFunction{}); el rewrite reserva 128B para CB_SAVE_REGS/CB_RESTORE_REGS. */
    out.cb_save_regs = cb_save_set;
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
     * futuros.  El boundary VM_ABI (params/RET/CALL) cruza por
     * proc->registers: el MOV generico de un vreg FP-class a/desde vm_reg_mem
     * se enruta a MOVSD/MOVSS en el rewrite (regalloc_rewrite.cpp, path
     * is_fp_operand), igual que un spill FP.  NOTA forward-compat: este gate y
     * el RegClass::FP son tambien la base de los futuros tipos anchos
     * N=potencia-de-2 (i128/f128 -> XMM, i256 -> YMM, i512 -> ZMM); el manejo
     * es guiado por ANCHO (fp_mov_for_width), no por un tipo float fijo. */
    /* fp_ok: float escalar en XMM.  Cualquier ISA salvo x87 (sin XMM) y x86-32
     * (ABI float distinto) -> SSE2/AVX/AVX512/AUTO.  Las binarias se emiten en
     * VX 3-op cuando vx_scalar (avx+); el resto (cvt/cmp/sqrt) sigue legacy
     * SSE por ahora (Increment 2b las pasa a VX para no mezclar). */
    const bool fp_ok = (abi == AbiKind::HOST_LEAF || abi == AbiKind::VM) &&
                       (fisa != FloatIsa::X87) && !mode32;
    /* Emitir las binarias escalares en VX 3-operandos no-destructivo (sin el
     * `mov` de coalescing) cuando el target es AVX/AVX512. */
    const bool vx_scalar =
        (fisa == FloatIsa::AVX || fisa == FloatIsa::AVX512F);
    /* Ancho del chunk SIMD a emitir en las ops VEC_* (vectorizacion).  AOT
     * (HOST_LEAF): lo fija el TARGET via --float-isa (cross-compile correcto: no
     * emitir AVX2 si el target es solo-SSE2; ancho SELECCIONABLE sse2->128/
     * avx->256/avx512->512).  Debe COINCIDIR con el chunk que horneo el matcher
     * (CompileOptions::aot_vec_width, mismo mapeo de --float-isa) -> chunk==
     * host_w en AOT -> la reduccion (acc de 1 reg, no splittea) cabe.  JIT
     * (VM_ABI): el host (vec_emit_isa), porque el .velb corre en ESTE host y se
     * descompone para portabilidad.  AUTO en AOT -> host del build como
     * estimacion (multiversion-cpuid futuro). */
    auto vec_host_w = [abi, fisa]() -> uint64_t {
        if (abi != AbiKind::HOST_LEAF)
            return jit::vec_isa_width(jit::vec_emit_isa());
        switch (fisa) {
        case FloatIsa::AVX: return 32u;     // YMM 256b
        case FloatIsa::AVX512F: return 64u; // ZMM 512b
        case FloatIsa::AUTO: return jit::vec_isa_width(jit::vec_isa_host());
        default: return 16u; // SSE2 / X87 -> XMM 128b
        }
    };
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
        if (vx::g_asm_backend == nullptr) {
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
            if (b.reg_auto) {
                // operando `reg` AUTO -> register-required (el RA elige
                // el fisico OPTIMO, no lo derrama).  El fisico se conoce
                // post-regalloc; marcamos binding_phys con un sentinel >=0 para
                // que la clasificacion in/out lo incluya (su intervalo cubre el
                // asm).  El $N se rellena en el rewrite con ra.reg_of.
                binding_phys[b.alloca_value] = 0; // sentinel "es binding" (no pin)
                out.set_vreg_reg_required(
                    static_cast<uint32_t>(b.alloca_value));
                continue;
            }
            const std::string canon = vx::asm_canonical_reg(b.reg);
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
                /* FP: overflow permitido -> stack arg.  El rewrite lo coloca
                 * tras los GP-stack en [rsp+base+(G+(fi-fmax))*8] (convencion
                 * Vesta-interna consistente con la carga del callee). */
                (void)fmax;
                OO.push_back(
                    MInstr::make_arg(static_cast<uint8_t>(fi_a), vrt(av)));
                ++fi_a;
            } else {
                /* GP: overflow permitido -> stack arg (args ilimitados).
                 * El rewrite lo coloca en [rsp+base+(idx-gmax)*8]. */
                (void)gmax;
                OO.push_back(
                    MInstr::make_arg(static_cast<uint8_t>(gi_a), vr(av)));
                ++gi_a;
            }
        }
        return true;
    };

    /* VM_ABI: store de UN arg a proc->registers.regs[slot].  CLAVE: un arg
     * FLOAT vive en un XMM; con vrt() (clase FP) el rewrite enruta el
     * `MOV regs[slot], fp_vreg` a MOVSD (guarda los 8 bytes = bits del f64).
     * Con vr() (GP) se guardaba un registro GP stale -> el callee leia bits
     * basura del arg float.  Espejo del fix de carga de params (vrt en el
     * prologo) y del retorno float (MOVQ_XMM_GP). */
    auto store_vm_arg = [&](std::vector<MInstr> &OO, int slot,
                            ir::IrValueId av) {
        const bool av_fp = fp_ok && av < fn.values.size() &&
                           ir_type_is_float(fn.values[av].type);
        OO.push_back(MInstr::make_unary(MOp::MOV, vm_reg_mem(slot),
                                        av_fp ? vrt(av) : vr(av)));
    };

    /* ---- P2: pre-pase de ADDRESSING-MODE FOLDING (disp) ----
     * Reconoce `ptr = ADD(base, const)` cuyo unico uso es como DIRECCION de
     * LOAD (acceso a campo `obj.field`, muy comun) y lo fusiona en un solo
     * `mov dst, [base + disp]` -- elimina el ADD separado.  Requisitos SOUND:
     *   - el const cabe en disp32;
     *   - @c ptr NO tiene ningun uso que NO sea direccion de LOAD (si se usa
     *     como valor o como direccion de STORE, se materializa normal -- el
     *     STORE-fold es un incremento posterior, su MInstr no tiene campo
     *     libre para el disp);
     *   - la base es HOST ptr (los LOAD de memoria VM bajan a LOAD_VM = call,
     *     que toma la direccion completa; no se fusiona).
     * fold_disp[ptr] = (base_vreg, disp).  El ADD de un ptr fusionado NO se
     * emite (queda muerto: su unico consumidor era la direccion).  Desactivable
     * con VESTA_NO_SIB=1. */
    std::unordered_map<uint32_t, std::pair<uint32_t, int64_t>> fold_disp;
    {
        static const bool sib_off = [] {
            const char *v = std::getenv("VESTA_NO_SIB");
            return v && v[0] != '\0' && v[0] != '0';
        }();
        const uint32_t NVAL = static_cast<uint32_t>(fn.values.size());
        /* La auto-vectorizacion (VEC_*) y MEMCPY usan valores sinteticos y
         * addressing complejo (offsets en el imm, punteros scratch) que el
         * fold no modela bien -> conservador: NO fusionar en funciones que
         * los usan.  El disp-fold de field-access (no vectorizado) se conserva. */
        bool has_complex_mem = false;
        for (const auto &blk : fn.blocks) {
            for (const ir::IrInstr &in : blk.instrs) {
                switch (in.op) {
                case ir::IrOp::VEC_ACC_ZERO:
                case ir::IrOp::VEC_ACC_ADD:
                case ir::IrOp::VEC_ACC_FMA:
                case ir::IrOp::VEC_ACC_COMBINE:
                case ir::IrOp::VEC_ACC_STORE:
                case ir::IrOp::VEC_BINOP:
                case ir::IrOp::VEC_BINOP_S:
                case ir::IrOp::VEC_FMA:
                case ir::IrOp::VEC_BCAST:
                case ir::IrOp::MEMCPY: has_complex_mem = true; break;
                default: break;
                }
                if (has_complex_mem) break;
            }
            if (has_complex_mem) break;
        }
        if (!sib_off && !has_complex_mem && NVAL > 0) {
            /* 1) valores constantes (para leer el offset). */
            std::unordered_map<uint32_t, int64_t> const_val;
            for (const auto &blk : fn.blocks)
                for (const ir::IrInstr &in : blk.instrs)
                    if (in.op == ir::IrOp::CONST && in.dst != ir::IR_NO_VALUE)
                        const_val[in.dst] = static_cast<int64_t>(in.imm);
            /* 2) candidatos ptr = ADD(base, const)  (const en cualquier lado). */
            std::unordered_map<uint32_t, std::pair<uint32_t, int64_t>> cand;
            for (const auto &blk : fn.blocks) {
                for (const ir::IrInstr &in : blk.instrs) {
                    if (in.op != ir::IrOp::ADD || in.dst == ir::IR_NO_VALUE ||
                        in.operands.size() != 2)
                        continue;
                    const uint32_t o0 = in.operands[0], o1 = in.operands[1];
                    auto c0 = const_val.find(o0), c1 = const_val.find(o1);
                    uint32_t base;
                    int64_t disp;
                    if (c1 != const_val.end() && c0 == const_val.end()) {
                        base = o0;
                        disp = c1->second;
                    } else if (c0 != const_val.end() && c1 == const_val.end()) {
                        base = o1;
                        disp = c0->second;
                    } else
                        continue;
                    if (disp < INT32_MIN || disp > INT32_MAX) continue;
                    /* solo HOST ptr (VM -> LOAD_VM, no se fusiona). */
                    if (in.dst >= NVAL || !fn.values[in.dst].is_host_ptr)
                        continue;
                    cand[in.dst] = {base, disp};
                }
            }
            /* 3) address-only: ptr solo puede usarse como direccion de LOAD.
             * Cualquier otro uso (valor, direccion de STORE, func_ptr) lo
             * descalifica. */
            std::unordered_set<uint32_t> disqualified;
            for (const auto &blk : fn.blocks) {
                for (const ir::IrInstr &in : blk.instrs) {
                    for (size_t k = 0; k < in.operands.size(); ++k) {
                        const bool is_load_addr =
                            (in.op == ir::IrOp::LOAD && k == 0);
                        if (!is_load_addr) disqualified.insert(in.operands[k]);
                    }
                    if (in.func_ptr != ir::IR_NO_VALUE)
                        disqualified.insert(in.func_ptr);
                    for (const ir::IrPhiArg &a : in.phi_args)
                        disqualified.insert(a.value);
                }
            }
            for (const auto &c : cand)
                if (!disqualified.count(c.first)) fold_disp.emplace(c);
            static const bool sdbg = [] {
                const char *v = std::getenv("VESTA_SIB_DBG");
                return v && v[0] != '\0' && v[0] != '0';
            }();
            if (sdbg)
                std::fprintf(stderr,
                             "[sib] %s: const=%zu cand=%zu fold=%zu\n",
                             fn.name.c_str(), const_val.size(), cand.size(),
                             fold_disp.size());
        }
    }

    for (size_t b = 0; b < NB; ++b) {
        const ir::IrBlock &ib = fn.blocks[b];
        MBlock mb;
        mb.label_id = blbl[b];
        auto &O = mb.instrs;

        /* ===== callback-ABI (jubilacion de slots): prologo nativo =====
         * Un callback llega por la convencion C del host (args en arg_regs), NO
         * en proc->registers.  Cargamos proc en RBX (TLS-direct gs:[disp]) y
         * marshaleamos los args nativos a proc->registers.regs[1..N] (+ argc en
         * R15) ANTES de que el prologo VM_ABI normal (abajo) los relea de ahi.
         * Modo SAFE (cuerpo no hoja-puro): salvamos proc->registers[0..15] a la
         * work-area del frame para re-entrancia, y los restauramos en cada RET.
         * v1: solo register-args + TLS-direct; call-fallback o stack-args (o
         * params float, cuya marshalizacion nativa->XMM aun no esta) -> bail a
         * slots (seguro).  El RET escribe el retorno en regs[0] Y en RAX. */
        if (cb_entry && b == 0) {
            const size_t np = fn.params.size();
            /* Call-fallback (tls_gs_disp == -1, p.ej. Linux/ELF donde el proc no
             * es gs-direct): LOAD_PROC carga proc via un CALL al stub que
             * PRESERVA los arg-regs (get_proc_addr = cb_preserving_get_proc).
             * Ya soportado -> no baila.  El marshalling (abajo) lee los args
             * intactos tras el LOAD_PROC. */
            /* Native arg regs por ABI.  GP (enteros/punteros) + XMM (floats).
             * Win64: la POSICION ordinal del arg es compartida entre bancos
             * (arg i -> GP_i O XMM_i, 4 slots; args 4+ en pila).  SysV: bancos
             * SEPARADOS (6 GP / 8 XMM); overflow de cualquier banco en pila. */
            static const MReg CB_WIN[] = {MReg::RCX, MReg::RDX, MReg::R8,
                                          MReg::R9};
            static const MReg CB_SYSV[] = {MReg::RDI, MReg::RSI, MReg::RDX,
                                           MReg::RCX, MReg::R8,  MReg::R9};
            static const MReg CB_XMM[] = {MReg::XMM0, MReg::XMM1, MReg::XMM2,
                                          MReg::XMM3, MReg::XMM4, MReg::XMM5,
                                          MReg::XMM6, MReg::XMM7};
            const MReg *cbr = target_sysv ? CB_SYSV : CB_WIN;
            /* Bail: (a) la convencion VM cabe en regs[1..12] + argc en regs[15]
             * (regs[13]/[14] reservados) -> >12 params no representable; (b)
             * tipos que no caben en un slot regs[] de 64-bit (SIMD >8 bytes,
             * hoy tratados como GP -> bail defensivo por tamano).  f32/f64 y
             * args por PILA SI se marshalean.  f32: el nativo lo pasa en XMM
             * low-32; MOVQ copia los 8 bytes (low-32 = bits f32) y el load
             * VM_ABI del param usa MOVSS (width=4 via vrt) -> lee solo esos 32
             * bits correctos (sin promocion CVTSS2SD). */
            if (np > 12) {
                vreg_dbg(fn.name.c_str(), "callback(argc>12)");
                return false;
            }
            for (size_t i = 0; i < np; ++i) {
                const ir::IrType pt = (fn.params[i] < fn.values.size())
                                          ? fn.values[fn.params[i]].type
                                          : ir::IrType::I64;
                if (ir_type_bytes(pt) > 8) {
                    vreg_dbg(fn.name.c_str(), "callback(wide-arg>8)");
                    return false;
                }
            }
            /* Cargar proc -> RBX via TLS-direct (gs:[disp]); no toca arg-regs. */
            const uint32_t proc_pool_idx = out.intern_imm64(cb.get_proc_addr);
            O.push_back(MInstr::make_load_proc(MReg::RBX, cb.tls_gs_disp,
                                               proc_pool_idx));
            /* Save-set (cuerpo no-hoja): salvar proc->registers[0..15] del
             * caller VM a la work-area del frame ANTES de marshalear (que pisa
             * regs[1..N]).  RBX ya = proc.  El rewrite reserva los 128B. */
            if (cb_save_set)
                O.push_back(MInstr::make_cb_save());
            /* Marshalear args nativos -> proc->regs[1..N].  Cada arg viene en un
             * reg (GP/XMM) o en la PILA del caller segun el ABI:
             *   Win64: posicion ordinal compartida; args 0..3 en reg, 4+ en
             *          [rbp + 48 + 8*(i-4)]  (rbp+8=retaddr, rbp+16..47=shadow).
             *   SysV : bancos separados (6 GP / 8 XMM); el overflow de cualquier
             *          banco va a [rbp + 16 + 8*j]  (j = orden de aparicion en
             *          pila; rbp+8=retaddr, sin shadow).
             * Float (f64): reg -> MOVQ (XMM->RAX); pila -> load 8 bytes.  En
             * ambos casos el valor termina en regs[i+1] como bit-pattern i64
             * (la VM_ABI lo relee via MOVSD).  RAX es scratch (no arg-reg).
             * RBP estable: has_stack_params (>arg_regs) fuerza el frame. */
            {
                size_t gpn = 0, fpn = 0, stk = 0;
                for (size_t i = 0; i < np; ++i) {
                    const ir::IrType pt = (fn.params[i] < fn.values.size())
                                              ? fn.values[fn.params[i]].type
                                              : ir::IrType::I64;
                    const bool isf = ir_type_is_float(pt); /* f32 o f64 */
                    bool on_stack = false;
                    int32_t stk_off = 0;
                    MReg srcreg = MReg::RAX;
                    if (!target_sysv) {
                        if (i < 4)
                            srcreg = isf ? CB_XMM[i] : CB_WIN[i];
                        else {
                            on_stack = true;
                            stk_off = 48 + 8 * static_cast<int32_t>(i - 4);
                        }
                    } else if (isf) {
                        if (fpn < 8)
                            srcreg = CB_XMM[fpn++];
                        else {
                            on_stack = true;
                            stk_off = 16 + 8 * static_cast<int32_t>(stk++);
                        }
                    } else {
                        if (gpn < 6)
                            srcreg = cbr[gpn++];
                        else {
                            on_stack = true;
                            stk_off = 16 + 8 * static_cast<int32_t>(stk++);
                        }
                    }
                    const MOperand dst = vm_reg_mem(static_cast<int>(i) + 1);
                    if (on_stack) {
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, MOperand::make_reg(MReg::RAX, 8),
                            MOperand::make_mem(MReg::RBP, stk_off)));
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, dst, MOperand::make_reg(MReg::RAX, 8)));
                    } else if (isf) {
                        O.push_back(MInstr::make_unary(
                            MOp::MOVQ_XMM_GP, MOperand::make_reg(MReg::RAX, 8),
                            MOperand::make_reg(srcreg, 8)));
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, dst, MOperand::make_reg(MReg::RAX, 8)));
                    } else {
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, dst, MOperand::make_reg(srcreg, 8)));
                    }
                }
            }
            /* argc -> regs[15]. */
            O.push_back(MInstr::make_unary(MOp::MOV, vm_reg_mem(15),
                                           MOperand::make_imm32(
                                               static_cast<int32_t>(np))));
        }

        /* VM_ABI: al entrar, cargar cada parametro desde
         * proc->registers.regs[i+1] (regs[0] reservado al retorno).
         * CLAVE: usar vrt() (class-aware), NO vr() (GP).  Un parametro FLOAT
         * en regs[i+1] guarda el bit-pattern i64 del f64; con vrt() el dst es
         * un vreg de clase FP -> el rewrite enruta el MOV a MOVSD (carga los 8
         * bytes = bits del f64 al XMM).  Con vr() (GP) se cargaba en un GP
         * register que ademas pisaba otro param (p.ej. el puntero p en RAX) ->
         * direccion basura en el loop -> SEGFAULT. */
        if (vm && b == 0) {
            for (size_t i = 0; i < fn.params.size(); ++i)
                O.push_back(
                    MInstr::make_unary(MOp::MOV, vrt(fn.params[i]),
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
            const size_t gmax_p = areg.size(), fmax_p = fareg.size();
            const int32_t shadow_p = (gmax_p == 4) ? 32 : 0; // home Win64
            auto is_fparam = [&](ir::IrValueId pv) -> bool {
                return fp_ok && pv < fn.values.size() &&
                       ir_type_is_float(fn.values[pv].type);
            };
            // G = nº de GP-stack params (los FP-stack van DESPUES en la pila).
            size_t gp_count = 0;
            for (size_t i = 0; i < fn.params.size(); ++i)
                if (!is_fparam(fn.params[i])) ++gp_count;
            const size_t G = (gp_count > gmax_p) ? (gp_count - gmax_p) : 0;
            // PASO 1: params en arg_regs (GP + FP) -> param-init reg.  Deben ir
            // PRIMERO: emit_host_param_loads consume estos lideres (vreg<-reg)
            // como parallel-move y se DETIENE en el primer load de pila.
            size_t gi_p = 0, fi_p = 0;
            for (size_t i = 0; i < fn.params.size(); ++i) {
                const ir::IrValueId pv = fn.params[i];
                if (is_fparam(pv)) {
                    if (fi_p < fmax_p)
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, vrt(pv),
                            MOperand::make_reg(
                                static_cast<MReg>(fareg[fi_p]), 8)));
                    ++fi_p;
                } else {
                    if (gi_p < gmax_p)
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, vrt(pv),
                            MOperand::make_reg(
                                static_cast<MReg>(areg[gi_p]), 8)));
                    ++gi_p;
                }
            }
            // PASO 2: params en PILA (overflow GP/FP).  El caller los dejo en
            // [rbp + 16 + shadow + k*8] (16 = saved rbp + ret; shadow = home
            // Win64; k = GP-stack-index, o G + FP-stack-index para los FP).
            // rbp estable (regalloc_rewrite fuerza el frame con stack-params).
            gi_p = 0;
            fi_p = 0;
            for (size_t i = 0; i < fn.params.size(); ++i) {
                const ir::IrValueId pv = fn.params[i];
                if (is_fparam(pv)) {
                    if (fi_p >= fmax_p) {
                        const int32_t off =
                            16 + shadow_p +
                            static_cast<int32_t>((G + (fi_p - fmax_p)) * 8);
                        O.push_back(MInstr::make_unary(
                            MOp::MOVSD, vrt(pv),
                            MOperand::make_mem(MReg::RBP, off)));
                    }
                    ++fi_p;
                } else {
                    if (gi_p >= gmax_p) {
                        const int32_t off =
                            16 + shadow_p +
                            static_cast<int32_t>((gi_p - gmax_p) * 8);
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, vrt(pv),
                            MOperand::make_mem(MReg::RBP, off)));
                    }
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
                        // vrt() (class-aware): para un PHI float (F32/F64) la
                        // copia debe ir por MOVSD/MOVSS (el rewrite la enruta
                        // por is_fp_operand); vr() la haria con MOV entero ->
                        // acumulador float loop-carried roto.
                        O.push_back(MInstr::make_unary(MOp::MOV, vrt(p.dst),
                                                       vrt(a.value)));
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

        bool sw_dense_term = false; // SWITCH_DENSE emitio el dispatch (island
                                    // de tabla); el resto del bloque (BST que
                                    // el frontend emite como path interp) es
                                    // dead code en JIT -> saltar.
        for (const ir::IrInstr &in : ib.instrs) {
            MOp mop;
            MCond cc;
            // Estampar la op anterior (sobrevive a `continue`) + snapshot.
            lm_flush();
            if (sw_dense_term) break; // dispatch denso ya emitido
            if (out.emit_line_map) {
                lm_before = O.size();
                lm_line = in.source_line;
                /* Identidad estable de la op IR = block_index*65536 + pos. */
                size_t pos = static_cast<size_t>(&in - ib.instrs.data());
                lm_ir_id = static_cast<uint32_t>(b * 65536u + pos);
            }
            if (in.op == ir::IrOp::PHI) continue; // resuelto via copias

            /* P2 SIB: el ADD que solo computa el disp de un LOAD fusionado NO
             * se emite (su unico consumidor era la direccion, que ahora usa
             * [base + disp] directo). */
            if (in.op == ir::IrOp::ADD && in.dst != ir::IR_NO_VALUE &&
                fold_disp.count(in.dst))
                continue;

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

            /* SWITCH_DENSE: jump table densa O(1) (computed-goto, lo mas optimo).
             * Dispatch: idx = tag - min; if idx u>= N -> default; else cargar la
             * direccion del brazo de la tabla y saltar (1 solo salto indirecto):
             *   mov  R10, tag ; sub R10, min ; cmp R10, N ; jae default
             *   lea  R11, [rip+table] ; mov R11, [R11 + R10*8] ; jmp R11
             *   table: dq arm0, arm1, ... (8B c/u, parchadas post-memcpy con la
             *          direccion nativa = base + label_offset del brazo).
             * El BST que el frontend emite tras este marker es el path del
             * interp + fallback; en JIT es dead code (saltamos el resto del
             * bloque via sw_dense_term).  Si falta algo, no-op -> corre el BST. */
            case ir::IrOp::SWITCH_DENSE: {
                flush_pending();
                const ir::IrBlockId def_blk = in.target_block;
                // VM_ABI (JIT) o HOST_LEAF (AOT): ambos emiten jump table.  En
                // AOT la tabla es SELF-RELATIVE (4B, PIC-safe, sin reloc); en
                // JIT es de punteros absolutos (8B, parchados con la addr
                // runtime).  Otros casos -> no-op (el BST hace el dispatch).
                const bool sw_host = (abi == AbiKind::HOST_LEAF);
                if ((!vm && !sw_host) || in.operands.empty() ||
                    in.jump_targets.empty() || def_blk == ir::IR_NO_BLOCK ||
                    def_blk >= blbl.size()) {
                    break;
                }
                const int64_t min_v =
                    static_cast<int64_t>(in.imm & 0xFFFFFFFFu);
                const bool no_bounds = ((in.imm >> 32) & 1u) != 0;
                const size_t range = in.jump_targets.size();
                const MReg RI = MReg::R10, RB = MReg::R11; // scratch reservados
                // idx = tag - min en RI.
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(RI, 8),
                                               vr(in.operands[0])));
                if (min_v != 0)
                    O.push_back(MInstr::make_binary(
                        MOp::SUB, MOperand::make_reg(RI, 8),
                        MOperand::make_reg(RI, 8),
                        MOperand::make_imm32(static_cast<int32_t>(min_v))));
                // bounds: cmp RI, range; jae default.  ELIDIDO cuando la tabla
                // cubre todo el rango del enum (no_bounds): el tag siempre es
                // valido -> idx in [0,range) garantizado.
                if (!no_bounds) {
                    MInstr c{};
                    c.op = MOp::CMP;
                    c.src1 = MOperand::make_reg(RI, 8);
                    c.src2 =
                        MOperand::make_imm32(static_cast<int32_t>(range));
                    O.push_back(c);
                    O.push_back(MInstr::make_jcc(MCond::AE, blbl[def_blk]));
                }
                const MLabelId table_lbl = out.new_label();
                O.push_back(MInstr::make_lea_label(MOperand::make_reg(RB, 8),
                                                   table_lbl));
                if (sw_host) {
                    // AOT self-relative.  Entrada 4B con signo = offset[arm] -
                    // offset[table].  Carga + sign-extend SIN MOVSX-de-memoria
                    // (el rewrite lo expandiria con scr1=R11, que colisiona con
                    // RB): (1) mov RI_d, [RB + RI*4] (MOV plano de 32b -> zero-
                    // ext, el rewrite lo respeta); (2) movsxd RI, RI_d (fuente
                    // REG -> el rewrite no toca memoria); (3) add RB, RI (RB =
                    // table_base + offset[arm]-offset[table] = func_base +
                    // offset[arm]); (4) jmp RB.
                    MInstr ld{};
                    ld.op = MOp::MOV;
                    ld.dst = MOperand::make_reg(RI, 4); // 32-bit -> zero-extiende
                    ld.src1 = MOperand::make_mem(RB, 0, RI, 4);
                    O.push_back(ld);
                    MInstr sx{};
                    sx.op = MOp::MOVSX;
                    sx.dst = MOperand::make_reg(RI, 8);
                    sx.src1 = MOperand::make_reg(RI, 4); // sign-extend low 32
                    O.push_back(sx);
                    O.push_back(MInstr::make_binary(
                        MOp::ADD, MOperand::make_reg(RB, 8),
                        MOperand::make_reg(RB, 8), MOperand::make_reg(RI, 8)));
                    O.push_back(MInstr::make_jmp_reg(RB));
                    // Tabla: entradas self-relative de 4 bytes.
                    O.push_back(MInstr::make_label_def(table_lbl));
                    for (size_t k = 0; k < range; ++k) {
                        const uint32_t tb = in.jump_targets[k];
                        const ir::IrBlockId arm =
                            (tb < blbl.size()) ? tb : def_blk;
                        O.push_back(MInstr::make_data_rel32_label(blbl[arm],
                                                                  table_lbl));
                        mb.extra_succs.push_back(static_cast<MBlockId>(arm));
                    }
                } else {
                    // JIT: jmp [RB + RI*8] (punteros absolutos de 8B parchados
                    // post-memcpy).  Funde load+jump en una instr.
                    MInstr j{};
                    j.op = MOp::JMP;
                    j.src1 = MOperand::make_mem(RB, 0, RI, 8);
                    O.push_back(j);
                    O.push_back(MInstr::make_label_def(table_lbl));
                    for (size_t k = 0; k < range; ++k) {
                        const uint32_t tb = in.jump_targets[k];
                        const ir::IrBlockId arm =
                            (tb < blbl.size()) ? tb : def_blk;
                        O.push_back(MInstr::make_data_ptr_label(blbl[arm]));
                        mb.extra_succs.push_back(static_cast<MBlockId>(arm));
                    }
                }
                mb.extra_succs.push_back(static_cast<MBlockId>(def_blk));
                sw_dense_term = true; // saltar el BST (dead code)
                break;
            }
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
                /* P3 imm-forms: operando CONST (i32) -> forma inmediata,
                 * evita materializar el const en un registro (y para IMUL
                 * ademas el mov 2-address).  IMUL -> `imul dst,x,imm` (3-op,
                 * rewrite dedicado); ADD/AND/OR/XOR -> `mov dst,x; OP dst,imm`
                 * (2-address, emit_alu 0x81/0x83).  Conmutatividad: const en
                 * cualquier lado salvo SUB (solo `x - const`; `const - x`
                 * necesitaria NEG -> no se fusiona). */
                {
                    const uint32_t a = in.operands[0], b = in.operands[1];
                    auto cst = [&](uint32_t v, int64_t &c) -> bool {
                        if (v < v_is_const.size() && v_is_const[v]) {
                            const int64_t cc = v_const[v];
                            if (cc >= INT32_MIN && cc <= INT32_MAX) {
                                c = cc;
                                return true;
                            }
                        }
                        return false;
                    };
                    const bool commut = (in.op != ir::IrOp::SUB);
                    int64_t c;
                    if (cst(b, c)) { // x OP const
                        O.push_back(MInstr::make_binary(
                            mop, vr(in.dst), vr(a),
                            MOperand::make_imm32(static_cast<int32_t>(c))));
                        break;
                    }
                    if (commut && cst(a, c)) { // const OP x -> x OP const
                        O.push_back(MInstr::make_binary(
                            mop, vr(in.dst), vr(b),
                            MOperand::make_imm32(static_cast<int32_t>(c))));
                        break;
                    }
                }
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
                if (vx_scalar) {
                    // VX 3-operandos no-destructivo (avx+): el rewrite NO mete
                    // el `mov dst,src1` -> menos instrucciones.
                    switch (in.op) {
                    case ir::IrOp::FADD:
                        fop = is_f32 ? MOp::VADDSS : MOp::VADDSD;
                        break;
                    case ir::IrOp::FSUB:
                        fop = is_f32 ? MOp::VSUBSS : MOp::VSUBSD;
                        break;
                    case ir::IrOp::FMUL:
                        fop = is_f32 ? MOp::VMULSS : MOp::VMULSD;
                        break;
                    default:
                        fop = is_f32 ? MOp::VDIVSS : MOp::VDIVSD;
                        break;
                    }
                } else {
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
                if (vx_scalar) O.back().flags |= MI_FLAG_VX_SCALAR;
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
                // avx: VX 3-op (VXORPS/VANDPS) -> sin legacy SSE mezclado.
                const MOp maskop =
                    vx_scalar ? (is_abs ? MOp::VANDPS : MOp::VXORPS)
                               : (is_abs ? MOp::ANDPS : MOp::XORPS);
                O.push_back(MInstr::make_binary(maskop, vrt(in.dst),
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
                if (vx_scalar) O.back().flags |= MI_FLAG_VX_SCALAR;
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
                if (vx_scalar) O.back().flags |= MI_FLAG_VX_SCALAR;
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
                if (vx_scalar) O.back().flags |= MI_FLAG_VX_SCALAR;
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
                if (vx_scalar) O.back().flags |= MI_FLAG_VX_SCALAR;
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
                if (vx_scalar) O.back().flags |= MI_FLAG_VX_SCALAR;
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
                /* variant: bit0 = MOD(1)/DIV(0), bit1 = UNSIGNED(2).  El tipo
                 * del IR decide signed vs unsigned (idiv/cqo vs div/xor). */
                dm.variant = (in.op == ir::IrOp::MOD) ? 1u : 0u;
                if (!ir_type_signed(in.type)) dm.variant |= 2u;
                O.push_back(dm);
                break;
            }
            /* NEG / NOT: x86 los tiene IN-PLACE (`neg rax` es rax = -rax), asi
             * que su forma maquina es de UN operando -- el encoder ni mira
             * src1.  Emitirlos como `NEG dst, src` con dst != src negaba lo que
             * hubiera en dst y tiraba el operando: `r.x = -this.x` dentro de un
             * metodo daba basura en JIT mientras el interprete daba lo correcto
             * (`0 - this.x`, que baja a SUB, si funcionaba).  Se copia primero
             * y se niega en sitio, como ya hacian ILOG2 y CTZ mas abajo. */
            case ir::IrOp::NEG:
            case ir::IrOp::NOT: {
                flush_pending();
                if (in.operands.size() != 1 || in.dst == ir::IR_NO_VALUE)
                    return false;
                const MOp mop = (in.op == ir::IrOp::NEG) ? MOp::NEG : MOp::NOT;
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               vr(in.operands[0])));
                O.push_back(MInstr::make_unary(mop, vr(in.dst), vr(in.dst)));
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
            /* UNWRAP (AOT/HOST_LEAF): assert non-null + passthrough.  Los
             * provably-non-null ya los elimino ir_pass_elide_unwrap (cero
             * codigo).  Para el resto: chequeo INLINE hiper-eficiente
             *   test v,v ; jne ok ; call __vx_panic_null ; ok: ; mov dst,v
             * Hot path = test + branch predicho-no-tomado (~0 overhead); el
             * panic es cold.  __vx_panic_null es un hook (default en la
             * bare-lib, redefinible -- freestanding lo provee el usuario). */
            case ir::IrOp::UNWRAP: {
                flush_pending();
                if (in.operands.empty() || in.dst == ir::IR_NO_VALUE) {
                    vreg_dbg(fn.name.c_str(), "unwrap-shape");
                    return false;
                }
                const ir::IrValueId v = in.operands[0];
                const MLabelId Lok = out.new_label();
                O.push_back(mk_test(v, v));
                O.push_back(MInstr::make_jcc(MCond::NE, Lok));
                if (abi == AbiKind::HOST_LEAF) {
                    /* AOT: hook __vx_panic_null (sin args; default bare-lib,
                     * redefinible -- freestanding lo provee el usuario). */
                    O.push_back(MInstr::make_call_sym(
                        out.intern_reloc_symbol("__vx_panic_null")));
                } else {
                    /* VM_ABI (JIT): camino frio llama vrt_unwrap_throw(proc)
                     * -> mismo FatalError capturable que el bytecode UNWRAP.
                     * proc vive en RBX.  Sin el fallback a slots, dec_or y
                     * cia compilan por vreg (consistencia de direccionamiento
                     * con sus callers -> retira slots). */
                    if (ent.unwrap_throw == 0) {
                        vreg_dbg(fn.name.c_str(), "unwrap-no-entry");
                        return false;
                    }
#if defined(_WIN32)
                    const MReg ca0 = MReg::RCX;
#else
                    const MReg ca0 = MReg::RDI;
#endif
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(ca0, 8),
                        MOperand::make_reg(MReg::RBX, 8)));
                    O.push_back(MInstr::make_call_abs(
                        out.intern_imm64(ent.unwrap_throw)));
                }
                /* Terminar el frame nativo tras el throw: vrt_unwrap_throw
                 * (do_throw) restaura RIP/RSP/RBP/regs del proceso al handler
                 * del catch (bytecode) y RETORNA.  Si el nativo cayera a Lok,
                 * ejecutaria con el estado VM ya restaurado -> deref invalido
                 * (crash determinista).  El epilogue (make_ret -> mov rsp,rbp +
                 * pop + ret) devuelve a enter_jit, que detecta rip!=pre_rip y
                 * deja que el interp resuma en el handler.  NO escribe regs[0]
                 * (do_throw ya lo puso = exception_ptr para el catch).
                 * En HOST_LEAF el __vx_panic_null normalmente aborta; el ret
                 * queda inalcanzable (inofensivo). */
                O.push_back(MInstr::make_ret());
                O.push_back(MInstr::make_label_def(Lok));
                O.push_back(
                    MInstr::make_unary(MOp::MOV, vr(in.dst), vr(v)));
                break;
            }
            /* ISNULL: dst = (src == 0) ? 1 : 0.  Inline puro, cero runtime:
             * test src,src (ZF<=>src==0) ; mov dst,0 (no toca flags) ;
             * setcc-E dst (byte bajo).  Mismo idiom que flush_pending para
             * materializar un bool. */
            case ir::IrOp::ISNULL: {
                flush_pending();
                if (in.operands.empty() || in.dst == ir::IR_NO_VALUE) {
                    vreg_dbg(fn.name.c_str(), "isnull-shape");
                    return false;
                }
                const ir::IrValueId v = in.operands[0];
                O.push_back(mk_test(v, v));
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               MOperand::make_imm32(0)));
                O.push_back(mk_setcc(in.dst, MCond::E)); // ZF <=> src==0
                break;
            }
            /* GETPID: PID encoded del proceso actual.  Es un servicio del VM
             * (vm->pid), no una op pura ni overridable -> CALL vrt_proc_pid.
             * Op raro (no hot).  AOT bare no tiene VM -> fallback. */
            case ir::IrOp::GETPID: {
                flush_pending();
                if (in.dst == ir::IR_NO_VALUE) break;
                if (abi == AbiKind::HOST_LEAF || ent.proc_pid == 0) {
                    vreg_dbg(fn.name.c_str(), "getpid-no-entry");
                    return false;
                }
#if defined(_WIN32)
                const MReg ca0 = MReg::RCX;
#else
                const MReg ca0 = MReg::RDI;
#endif
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(ca0, 8),
                                               MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.proc_pid)));
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               MOperand::make_reg(MReg::RAX, 8)));
                break;
            }
            /* MVTAKE_IR: move-and-zero (ownership de smart pointers).
             * [dst] = [src]; [src] = 0.  Inline puro (3 ops de memoria host)
             * cuando ambas direcciones son host_ptr (slots de unique/shared
             * en el frame host).  VM-addr -> fallback (raro). */
            /* Atomicas del lenguaje (Phase Z / FN.4): a instrucciones x86
             * atomicas nativas.  Buen uso de registros: CAS solo fija RAX
             * (obligado por la ISA de cmpxchg) via precoloreo de un temp;
             * addr/desired quedan LIBRES para el allocator.  ADD (xadd) no
             * fija ningun registro (dst in/out estilo 2-address). */
            case ir::IrOp::ATOMIC_LD_I64: {
                flush_pending();
                if (in.operands.size() != 1 || in.dst == ir::IR_NO_VALUE)
                    return false;
                /* mov dst, [addr] (load 64-bit alineado = atomico en x86). */
                O.push_back(
                    MInstr::make_load(vr(in.dst), vr(in.operands[0]), 8, false));
                break;
            }
            case ir::IrOp::ATOMIC_ST_I64: {
                flush_pending();
                if (in.operands.size() != 2) return false;
                /* mov [addr], val (store alineado; release en x86-TSO). */
                O.push_back(MInstr::make_store(vr(in.operands[0]),
                                               vr(in.operands[1]), 8));
                break;
            }
            case ir::IrOp::ATOMIC_ADD_I64: {
                flush_pending();
                if (in.operands.size() != 2 || in.dst == ir::IR_NO_VALUE)
                    return false;
                /* dst = delta; lock xadd [addr], dst (dst = valor viejo). */
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               vr(in.operands[1])));
                MInstr xa{};
                xa.op = MOp::ATOMICADD_V;
                xa.dst = vr(in.dst);          /* in/out: delta -> old */
                xa.src1 = vr(in.operands[0]); /* addr */
                O.push_back(xa);
                break;
            }
            case ir::IrOp::ATOMIC_CAS_I64: {
                flush_pending();
                if (in.operands.size() != 3 || in.dst == ir::IR_NO_VALUE)
                    return false;
                /* rax_v = expected (precoloreado a RAX, obligado por cmpxchg);
                 * lock cmpxchg [addr], desired; dst = rax_v (viejo). */
                const ir::IrValueId rax_v = new_tmp();
                out.set_vreg_fixed(static_cast<uint32_t>(rax_v),
                                   static_cast<uint8_t>(MReg::RAX));
                O.push_back(MInstr::make_unary(MOp::MOV, vr(rax_v),
                                               vr(in.operands[1])));
                MInstr cx{};
                cx.op = MOp::ATOMICCAS_V;
                cx.dst = vr(rax_v);           /* in/out: expected -> old (RAX) */
                cx.src1 = vr(in.operands[0]); /* addr */
                cx.src2 = vr(in.operands[2]); /* desired */
                O.push_back(cx);
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               vr(rax_v)));
                break;
            }
            case ir::IrOp::MVTAKE_IR: {
                flush_pending();
                if (in.operands.size() < 2) {
                    vreg_dbg(fn.name.c_str(), "mvtake-shape");
                    return false;
                }
                const ir::IrValueId mv_dst = in.operands[0];
                const ir::IrValueId mv_src = in.operands[1];
                const bool mv_host = !vm || (fn.values[mv_dst].is_host_ptr &&
                                             fn.values[mv_src].is_host_ptr);
                const ir::IrValueId mv_tmp = new_tmp();
                const ir::IrValueId mv_z = new_tmp();
                if (mv_host) {
                    /* Ambas host: 3 ops de memoria directas. */
                    O.push_back(
                        MInstr::make_load(vr(mv_tmp), vr(mv_src), 8, false));
                    O.push_back(MInstr::make_store(vr(mv_dst), vr(mv_tmp), 8));
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(mv_z),
                                                   MOperand::make_imm32(0)));
                    O.push_back(MInstr::make_store(vr(mv_src), vr(mv_z), 8));
                } else {
                    /* VM-addr (slots de smart pointers en VM-stack): page-cache
                     * inline via LOAD_VM/STORE_VM (runtime solo en page-miss). */
                    if (ent.vm_read_u64 == 0 || ent.vm_write_u64 == 0) {
                        vreg_dbg(fn.name.c_str(), "mvtake(no-vm-rt)");
                        return false;
                    }
                    const uint32_t rd = out.intern_imm64(ent.vm_read_u64);
                    const uint32_t wr = out.intern_imm64(ent.vm_write_u64);
                    O.push_back(MInstr::make_load_vm(vr(mv_tmp), vr(mv_src), 8,
                                                     false, rd));
                    O.push_back(
                        MInstr::make_store_vm(vr(mv_dst), vr(mv_tmp), 8, wr));
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(mv_z),
                                                   MOperand::make_imm32(0)));
                    O.push_back(
                        MInstr::make_store_vm(vr(mv_src), vr(mv_z), 8, wr));
                }
                break;
            }
            /* NEWOBJS: alloc en SharedHeap -> handle.  vrt_newobjs(proc, cls).
             * Op raro (memoria compartida); el alloc shared (slab+CAS) no se
             * inlinea -> CALL directo es lo optimo.  Devuelve handle directo
             * (sin gcderef extra). */
            case ir::IrOp::NEWOBJS: {
                flush_pending();
                if (!vm || ent.newobjs == 0 || in.operands.empty() ||
                    in.dst == ir::IR_NO_VALUE) {
                    vreg_dbg(fn.name.c_str(), "newobjs(no-vm/no-addr)");
                    return false;
                }
#if defined(_WIN32)
                const MReg ns_pr = MReg::RCX, ns_cls = MReg::RDX;
#else
                const MReg ns_pr = MReg::RDI, ns_cls = MReg::RSI;
#endif
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(ns_cls, 8),
                                               vr(in.operands[0])));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(ns_pr, 8),
                                               MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(MInstr::make_call_abs(out.intern_imm64(ent.newobjs)));
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               MOperand::make_reg(MReg::RAX, 8)));
                break;
            }
            /* DLOPEN: carga libreria (syscall del SO).  vrt_dlopen(proc,
             * path_addr, path_len).  No inlineable (OS call); raro. */
            case ir::IrOp::DLOPEN: {
                flush_pending();
                if (!vm || ent.dlopen == 0 || in.operands.size() < 2 ||
                    in.dst == ir::IR_NO_VALUE) {
                    vreg_dbg(fn.name.c_str(), "dlopen(no-vm/no-addr)");
                    return false;
                }
#if defined(_WIN32)
                const MReg dl_pr = MReg::RCX, dl_a1 = MReg::RDX,
                           dl_a2 = MReg::R8;
#else
                const MReg dl_pr = MReg::RDI, dl_a1 = MReg::RSI,
                           dl_a2 = MReg::RDX;
#endif
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::R10, 8),
                    vr(in.operands[0])));
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::R11, 8),
                    vr(in.operands[1])));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(dl_a1, 8),
                                               MOperand::make_reg(MReg::R10, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(dl_a2, 8),
                                               MOperand::make_reg(MReg::R11, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(dl_pr, 8),
                                               MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(MInstr::make_call_abs(out.intern_imm64(ent.dlopen)));
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               MOperand::make_reg(MReg::RAX, 8)));
                break;
            }
            /* PANIC: FatalError USER_ABORT (terminal -- no retorna).
             * vrt_panic_str(proc, msg_addr, msg_len).  Frio por definicion;
             * CALL directo es lo optimo (inlinear codigo terminal no aporta). */
            case ir::IrOp::PANIC: {
                flush_pending();
                if (!vm || ent.panic_str == 0 || in.operands.size() < 2) {
                    vreg_dbg(fn.name.c_str(), "panic(no-vm/no-addr)");
                    return false;
                }
#if defined(_WIN32)
                const MReg pa_pr = MReg::RCX, pa_a1 = MReg::RDX,
                           pa_a2 = MReg::R8;
#else
                const MReg pa_pr = MReg::RDI, pa_a1 = MReg::RSI,
                           pa_a2 = MReg::RDX;
#endif
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::R10, 8),
                    vr(in.operands[0])));
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::R11, 8),
                    vr(in.operands[1])));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(pa_a1, 8),
                                               MOperand::make_reg(MReg::R10, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(pa_a2, 8),
                                               MOperand::make_reg(MReg::R11, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(pa_pr, 8),
                                               MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.panic_str)));
                break;
            }
            /* LANDINGPAD: primera op del bloque catch in-JIT.  do_throw deja
             * la excepcion en proc->registers[0]; el catch la lee de ahi. */
            case ir::IrOp::LANDINGPAD: {
                flush_pending();
                if (!vm || in.dst == ir::IR_NO_VALUE) {
                    vreg_dbg(fn.name.c_str(), "landingpad(no-vm)");
                    return false;
                }
                O.push_back(
                    MInstr::make_unary(MOp::MOV, vr(in.dst), vm_reg_mem(0)));
                break;
            }
            /* TRYENTER (in-JIT, Opcion B): registra un ExceptionFrame con la
             * direccion NATIVA del bloque catch (LEA_LABEL al label del
             * handler) + handoff de rsp/rbp host.  El catch (in.target_block)
             * corre EN JIT; un throw que lo matchee resume via vrt_resume_jit
             * (do_throw) -- sin traduccion al interp.  El edge abnormal
             * tryenter->catch se registra en extra_succs para que la liveness
             * mantenga vivos (force-spill) los valores que el catch usa. */
            case ir::IrOp::TRYENTER: {
                flush_pending();
                const ir::IrBlockId hb = in.target_block;
                if (!vm || ent.tryenter_jit == 0 || in.operands.size() < 2 ||
                    hb == ir::IR_NO_BLOCK || hb >= blbl.size() ||
                    ent.jit_exc_rsp_off < 0 || ent.jit_exc_rbp_off < 0) {
                    vreg_dbg(fn.name.c_str(), "tryenter(no-vm/no-entry)");
                    return false;
                }
                /* imm=1: el catch puede capturar un AV de OS (catch-all o
                 * FatalError) -> in-JIT inseguro (av_recovery clobbea los slots
                 * del frame JIT antes del resume).  Bail -> la fn corre en
                 * interp (correcto).  Ver nota en lower_try. */
                if (in.imm != 0) {
                    vreg_dbg(fn.name.c_str(), "tryenter-av-catchable");
                    return false;
                }
#if defined(_WIN32)
                const MReg te_a0 = MReg::RCX, te_a1 = MReg::RDX,
                           te_a2 = MReg::R8;
#else
                const MReg te_a0 = MReg::RDI, te_a1 = MReg::RSI,
                           te_a2 = MReg::RDX;
#endif
                /* handoff: proc->jit_exc_rsp/rbp = rsp/rbp host (frame estable
                 * del try; vrt_resume_jit los restaurara). */
                O.push_back(MInstr::make_unary(
                    MOp::MOV,
                    MOperand::make_mem(MReg::RBX, ent.jit_exc_rsp_off),
                    MOperand::make_reg(MReg::RSP, 8)));
                O.push_back(MInstr::make_unary(
                    MOp::MOV,
                    MOperand::make_mem(MReg::RBX, ent.jit_exc_rbp_off),
                    MOperand::make_reg(MReg::RBP, 8)));
                /* catch native addr -> R10 (scratch staging, igual que DLOPEN);
                 * type (op1) -> R11.  Luego mover a arg regs + proc=RBX. */
                O.push_back(MInstr::make_lea_label(
                    MOperand::make_reg(MReg::R10, 8), blbl[hb]));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R11, 8),
                                               vr(in.operands[1])));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(te_a1, 8),
                                               MOperand::make_reg(MReg::R11, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(te_a2, 8),
                                               MOperand::make_reg(MReg::R10, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(te_a0, 8),
                                               MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.tryenter_jit)));
                /* Edge abnormal: el catch es sucesor (via runtime) del bloque
                 * del tryenter -> liveness/force-spill. */
                mb.extra_succs.push_back(static_cast<MBlockId>(hb));
                break;
            }
            /* TRYLEAVE: salida normal del try -> pop del ExceptionFrame.
             * CALL vrt_tryleave(proc). */
            case ir::IrOp::TRYLEAVE: {
                flush_pending();
                if (!vm || ent.tryleave == 0) {
                    vreg_dbg(fn.name.c_str(), "tryleave(no-vm/no-entry)");
                    return false;
                }
#if defined(_WIN32)
                const MReg tl_a0 = MReg::RCX;
#else
                const MReg tl_a0 = MReg::RDI;
#endif
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(tl_a0, 8),
                                               MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.tryleave)));
                break;
            }
            /* THROW: lanza una excepcion user-defined.  CALL vrt_throw_user;
             * do_throw resume el catch (in-JIT via vrt_resume_jit -> no retorna;
             * o cross-function/uncaught -> retorna y el make_ret devuelve limpio
             * a enter_jit).  No escribe regs[0] (do_throw lo pone). */
            case ir::IrOp::THROW: {
                flush_pending();
                /* AOT/Embed (HOST_LEAF, sin VM): el throw baja a CALL al
                 * runtime auto-hospedado __vx_throw(value) (vx_exc.vx,
                 * setjmp/longjmp).  noreturn -> RET de relleno para cerrar
                 * el bloque (nunca se ejecuta; el longjmp diverge). */
                if (abi == AbiKind::HOST_LEAF) {
                    if (in.operands.empty()) {
                        vreg_dbg(fn.name.c_str(), "throw(host-leaf-no-arg)");
                        return false;
                    }
                    // __vx_throw(value[, type_id]): operands[1] (type-id del
                    // intervalo) presente en native_poo para el type matching.
                    std::vector<ir::IrValueId> targs = {in.operands[0]};
                    if (in.operands.size() >= 2)
                        targs.push_back(in.operands[1]);
                    if (!emit_host_args(targs, O)) {
                        vreg_dbg(fn.name.c_str(), "throw(host-leaf-args)");
                        return false;
                    }
                    O.push_back(MInstr::make_call_sym(
                        out.intern_reloc_symbol("__vx_throw")));
                    O.push_back(MInstr::make_ret());
                    break;
                }
                if (!vm || ent.throw_user == 0 || in.operands.empty()) {
                    vreg_dbg(fn.name.c_str(), "throw(no-vm/no-entry)");
                    return false;
                }
#if defined(_WIN32)
                const MReg tw_a0 = MReg::RCX, tw_a1 = MReg::RDX;
#else
                const MReg tw_a0 = MReg::RDI, tw_a1 = MReg::RSI;
#endif
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(MReg::R10, 8),
                                               vr(in.operands[0])));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(tw_a1, 8),
                                               MOperand::make_reg(MReg::R10, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(tw_a0, 8),
                                               MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.throw_user)));
                O.push_back(MInstr::make_ret());
                break;
            }
            /* STRCONV: convierte StringObject a otra codificacion.
             * vrt_str_conv(proc, str, enc).  operands[0]=str, imm=enc.
             * Conversion compleja (UTF-8/16/32) -> CALL directo es lo optimo. */
            case ir::IrOp::STRCONV: {
                flush_pending();
                if (!vm || ent.str_conv == 0 || in.operands.empty() ||
                    in.dst == ir::IR_NO_VALUE) {
                    vreg_dbg(fn.name.c_str(), "strconv(no-vm/no-addr)");
                    return false;
                }
#if defined(_WIN32)
                const MReg sc_pr = MReg::RCX, sc_s = MReg::RDX,
                           sc_e = MReg::R8;
#else
                const MReg sc_pr = MReg::RDI, sc_s = MReg::RSI,
                           sc_e = MReg::RDX;
#endif
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(sc_s, 8),
                                               vr(in.operands[0])));
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(sc_e, 8),
                    MOperand::make_imm32(static_cast<int32_t>(in.imm))));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(sc_pr, 8),
                                               MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.str_conv)));
                O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                               MOperand::make_reg(MReg::RAX, 8)));
                break;
            }
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
                // Fall-through: los bloques se emiten en orden 0..NB, asi que un
                // BR al bloque SIGUIENTE (b+1) es un `jmp` a la instruccion que
                // le sigue -> redundante.  Se omite (cae por fall-through).
                if (t != b + 1) {
                    O.push_back(MInstr::make_jmp(blbl[t]));
                }
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

                // La rama FALSE cae por fall-through si su target es el bloque
                // siguiente (b+1) -> se omite el `jmp` redundante.
                const bool false_fallthrough = (tf == b + 1);
                if (has_pend && pend_dst == cond &&
                    vreg_count_uses(fn, pend_dst) == 1) {
                    /* FUSION: CMP a,b + Jcc(cc) true + JMP false.  Solo si el
                     * bool del CMP se usa UNICAMENTE en este BR_COND (si no,
                     * hay que materializarlo via SETcc para el otro uso). */
                    O.push_back(mk_cmp(pend_a, pend_b));
                    O.push_back(MInstr::make_jcc(pend_cc, blbl[tt]));
                    if (!false_fallthrough) {
                        O.push_back(MInstr::make_jmp(blbl[tf]));
                    }
                    has_pend = false;
                } else {
                    flush_pending();
                    O.push_back(mk_test(cond, cond));
                    O.push_back(MInstr::make_jcc(MCond::NE, blbl[tt]));
                    if (!false_fallthrough) {
                        O.push_back(MInstr::make_jmp(blbl[tf]));
                    }
                }
                mb.succ_a = static_cast<MBlockId>(tt);
                mb.succ_b = static_cast<MBlockId>(tf);
                break;
            }

            case ir::IrOp::RET: {
                flush_pending();
                if (!in.operands.empty()) {
                    const ir::IrValueId rv = in.operands[0];
                    const bool rv_fp = fp_ok && rv < fn.values.size() &&
                                       ir_type_is_float(fn.values[rv].type);
                    /* Float return HOST_LEAF (Phase AOT C1): el valor va a XMM0
                     * (ret_reg[FP]).  El MOV XMM0 <- vreg_fp lo enruta el rewrite
                     * a MOVSD (is_fp_operand). */
                    if (!vm && rv_fp) {
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, MOperand::make_reg(MReg::XMM0, 8),
                            vrt(rv)));
                        O.push_back(MInstr::make_ret());
                        break;
                    }
                    /* Float return VM_ABI: proc->registers.regs[0] guarda el
                     * bit-pattern i64 del f64 (convencion del interp).  El valor
                     * vive en un XMM -> MOVQ_XMM_GP mueve los bits a un GP temp y
                     * luego se escribe a regs[0].  Sin esto, vr(rv) trataba el
                     * valor float como GP y movia un registro stale (basura) a
                     * regs[0] -> retorno incorrecto (p.ej. 0). */
                    if (vm && rv_fp) {
                        const ir::IrValueId gpb = new_tmp();
                        O.push_back(MInstr::make_unary(MOp::MOVQ_XMM_GP,
                                                       vr(gpb), vrt(rv)));
                        O.push_back(MInstr::make_unary(MOp::MOV, vm_reg_mem(0),
                                                       vr(gpb)));
                        /* Callback: el retorno nativo float va en XMM0. */
                        if (cb_entry)
                            O.push_back(MInstr::make_unary(
                                MOp::MOV, MOperand::make_reg(MReg::XMM0, 8),
                                vrt(rv)));
                        /* Save-set: restaurar regs[0..15] del caller (no toca
                         * RAX/XMM0 -> el retorno nativo sobrevive). */
                        if (cb_save_set) O.push_back(MInstr::make_cb_restore());
                        O.push_back(MInstr::make_ret());
                        break;
                    }
                    /* GP: VM_ABI -> regs[0] ([rbx+off]); host leaf -> RAX. */
                    const MOperand dst =
                        vm ? vm_reg_mem(0) : MOperand::make_reg(MReg::RAX, 8);
                    O.push_back(
                        MInstr::make_unary(MOp::MOV, dst, vr(in.operands[0])));
                    /* Callback: el retorno nativo (entero/ptr) va en RAX ademas
                     * de regs[0] (que lee el interp). */
                    if (cb_entry && vm)
                        O.push_back(MInstr::make_unary(
                            MOp::MOV, MOperand::make_reg(MReg::RAX, 8),
                            vr(in.operands[0])));
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
                /* Save-set: restaurar regs[0..15] del caller antes del ret.
                 * RAX (retorno nativo del callback) ya esta escrito y el
                 * restore usa R11 -> no lo pisa. */
                if (cb_save_set) O.push_back(MInstr::make_cb_restore());
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
                const int w = ir_type_bytes(in.type);
                const bool sgn = ir_type_signed(in.type);
                /* AOT (HOST_LEAF): NO hay vm_mem -> toda direccion es host
                 * (el str_lit_addr/.rodata, malloc, alloca son host_ptr
                 * reales).  Solo el VM_ABI traduce vaddr -> host via
                 * LOAD_VM; en bare emitimos LOAD host directo. */
                if (vm && !fn.values[in.operands[0]].is_host_ptr) {
                    /* float desde memoria VM (vaddr): LOAD_VM no materializa a
                     * XMM aun -> bail.  El caso HOST si se soporta (make_load
                     * + rewrite enruta el dst FP a MOVSD/MOVSS). */
                    if (ir_type_is_float(in.type)) {
                        vreg_dbg(fn.name.c_str(), "load-float-vm");
                        return false;
                    }
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
                 * rewrite emite `mov r32,[mem]` (zero-extend por hardware).
                 * El DST usa vrt() (class-aware): para un f64/f32 le da clase
                 * FP -> el rewrite enruta a MOVSD/MOVSS (sin esto, vr() lo
                 * hardcodea a GP y el valor float queda inconsistente con el
                 * FADD que SI lo trata como XMM -> codegen roto). */
                /* P2 SIB: si el ptr es `base + const` (address-only), emitir
                 * `mov dst, [base + disp]` (el disp viaja en src2=IMM32; el
                 * rewrite lo usa en make_mem).  Si no, LOAD normal [ptr]. */
                auto fd = fold_disp.find(in.operands[0]);
                if (fd != fold_disp.end()) {
                    MInstr ld = MInstr::make_load(vrt(in.dst),
                                                  vr(fd->second.first),
                                                  static_cast<uint8_t>(w), sgn);
                    ld.src2 = MOperand::make_imm32(
                        static_cast<int32_t>(fd->second.second));
                    O.push_back(ld);
                    break;
                }
                O.push_back(MInstr::make_load(vrt(in.dst), vr(in.operands[0]),
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
                const int w = ir_type_bytes(in.type);
                /* AOT (HOST_LEAF): toda direccion es host -> STORE directo
                 * (ver nota en LOAD).  Solo VM_ABI usa STORE_VM. */
                if (vm && !fn.values[in.operands[1]].is_host_ptr) {
                    /* float a memoria VM (vaddr): STORE_VM no soporta XMM aun
                     * -> bail.  El caso HOST si (make_store + rewrite MOVSD). */
                    if (ir_type_is_float(in.type)) {
                        vreg_dbg(fn.name.c_str(), "store-float-vm");
                        return false;
                    }
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
                /* El VALOR (operands[0]) usa vrt() (class-aware): para f64/f32
                 * le da clase FP -> el rewrite enruta a MOVSD/MOVSS [addr],xmm
                 * (consistente con como el FADD produjo el valor en XMM). */
                O.push_back(MInstr::make_store(vr(in.operands[1]),
                                               vrt(in.operands[0]),
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

            /* VEC_UNOP dst[i] = OP a[i] (auto-vectorizacion unaria).  SIMD
             * packed 128b (W=2 f64): MOVUPD x0,[a]; <op> x0; MOVUPD [dst],x0.
             *   copy(0) -> sin op; sqrt(3) -> SQRTPD x0,x0;
             *   fneg(1) -> XORPD x0,mask(0x8000..);  fabs(2) -> ANDPD x0,mask(0x7fff..).
             * La mascara de signo de 16B se construye desde un imm64 -> GP ->
             * MOVQ_GP_XMM x1 -> UNPCKLPD x1,x1 (difunde el lane bajo a ambos),
             * sin constante en memoria.  Scratch del target (gp0/gp1, fp0/fp1),
             * igual que VEC_BINOP (clobber set formal). */
            case ir::IrOp::VEC_UNOP: {
                flush_pending();
                if (in.operands.size() != 2) return false;
                const uint64_t chunk_w = in.imm & 0xFF; // 16/32/64
                const uint64_t subop = (in.imm >> 8) & 0xFF;
                if (chunk_w != 16 && chunk_w != 32 && chunk_w != 64)
                    return false;
                if (!fp_ok || in.type != ir::IrType::F64) return false; // f64
                /* mismo chunk/descompone que VEC_BINOP: emite al ancho del host
                 * (eff_w), descomponiendo el chunk en n_pieces. */
                const uint64_t host_w =
                    vec_host_w();
                const uint64_t eff_w = (chunk_w < host_w) ? chunk_w : host_w;
                const uint64_t n_pieces = chunk_w / eff_w;
                const auto &gpsc =
                    tri_sel.scratch[static_cast<size_t>(RegClass::GP)];
                const auto &fpsc =
                    tri_sel.scratch[static_cast<size_t>(RegClass::FP)];
                if (gpsc.size() < 2 || fpsc.size() < 2) return false;
                const MReg gp0 = static_cast<MReg>(gpsc[0]);
                const MReg gp1 = static_cast<MReg>(gpsc[1]);
                const MReg fp0 = static_cast<MReg>(fpsc[0]);
                const MReg fp1 = static_cast<MReg>(fpsc[1]);
                const uint8_t ew = static_cast<uint8_t>(eff_w);
                const MOperand x0 = MOperand::make_reg(fp0, ew);
                const MOperand x1 = MOperand::make_reg(fp1, ew); // mascara wide
                /* mascara de signo wide en x1 (solo fneg/fabs).  Se construye
                 * UNA vez al ancho eff_w: imm64 -> GP -> MOVQ_GP_XMM x1.lo ->
                 * difundir a todos los lanes (UNPCKLPD para 128b, VBROADCASTSD
                 * para YMM/ZMM).  Vive a traves del bucle de piezas (los MOV de
                 * base son GP, no tocan el scratch FP). */
                if (subop == 1 || subop == 2) {
                    const uint64_t mask = (subop == 1) ? 0x8000000000000000ULL
                                                       : 0x7fffffffffffffffULL;
                    const uint32_t idx = out.intern_imm64(mask);
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(gp1, 8),
                        MOperand::make_imm64_idx(idx)));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVQ_GP_XMM, MOperand::make_reg(fp1, 16),
                        MOperand::make_reg(gp1, 8)));
                    if (eff_w <= 16)
                        O.push_back(MInstr::make_unary(
                            MOp::UNPCKLPD, MOperand::make_reg(fp1, 16),
                            MOperand::make_reg(fp1, 16)));
                    else
                        O.push_back(MInstr::make_unary(
                            MOp::VBROADCASTSD, x1,
                            MOperand::make_reg(fp1, 16)));
                }
                for (uint64_t pc = 0; pc < n_pieces; ++pc) {
                    const int32_t off = static_cast<int32_t>(pc * eff_w);
                    // load a -> x0
                    O.push_back(MInstr::make_unary(MOp::MOV,
                                                   MOperand::make_reg(gp0, 8),
                                                   vr(in.operands[1])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD, x0, MOperand::make_mem(gp0, off)));
                    // op
                    if (subop == 0) {
                        /* copy: nada */
                    } else if (subop == 1) {
                        O.push_back(MInstr::make_unary(MOp::XORPD, x0, x1));
                    } else if (subop == 2) {
                        O.push_back(MInstr::make_unary(MOp::ANDPD, x0, x1));
                    } else if (subop == 3) {
                        O.push_back(MInstr::make_unary(MOp::SQRTPD, x0, x0));
                    } else {
                        return false;
                    }
                    // store -> dst
                    O.push_back(MInstr::make_unary(MOp::MOV,
                                                   MOperand::make_reg(gp0, 8),
                                                   vr(in.operands[0])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD, MOperand::make_mem(gp0, off), x0));
                }
                break;
            }

            /* VEC_BINOP dst[i] = a[i] OP b[i] (auto-vectorizacion).  SIMD
             * packed: MOVUPD xmm0,[a]; MOVUPD xmm1,[b]; <ADDPD/...> xmm0,xmm1;
             * MOVUPD [dst],xmm0.  Solo 128b (W=2 f64) por ahora.  Robusto:
             * save/restore de xmm0/xmm1 en pila (por si el FP-regalloc los
             * tiene vivos) y dst/a/b a R10/R11 (scratch GP no asignable). */
            case ir::IrOp::VEC_BINOP: {
                flush_pending();
                if (in.operands.size() != 3) return false;
                const uint64_t chunk_w = in.imm & 0xFF; // 16/32/64 (matcher)
                const uint64_t subop = (in.imm >> 8) & 0xFF;
                if (chunk_w != 16 && chunk_w != 32 && chunk_w != 64)
                    return false;
                /* El matcher horneo un chunk de @c chunk_w bytes; lo emitimos
                 * con el ancho SIMD del HOST (16=SSE2/32=AVX2/64=AVX512),
                 * descomponiendo en @c n_pieces ops si el chunk es mas ancho.
                 * Asi el `.velb` es portable (corre en cualquier x86-64) y
                 * AUTO-cpuid.  VESTA_JIT_VEC_ISA fuerza el ancho (validar
                 * AVX512 por disasm en CPU sin avx512). */
                const uint64_t host_w =
                    vec_host_w();
                const uint64_t eff_w = (chunk_w < host_w) ? chunk_w : host_w;
                const uint64_t n_pieces = chunk_w / eff_w;
                /* op packed segun el tipo de elemento: float (ADDPD/...) o
                 * entero (PADDD/PSUBD i32, PADDQ/PSUBQ i64).  No hay mul/div
                 * entero packed en SSE2 -> bail (la cola/loop escalar lo hace). */
                MOp pop;
                if (in.type == ir::IrType::F64) {
                    pop = (subop == 0)   ? MOp::ADDPD
                          : (subop == 1) ? MOp::SUBPD
                          : (subop == 2) ? MOp::MULPD
                                         : MOp::DIVPD;
                } else if (in.type == ir::IrType::F32) {
                    pop = (subop == 0)   ? MOp::ADDPS
                          : (subop == 1) ? MOp::SUBPS
                          : (subop == 2) ? MOp::MULPS
                                         : MOp::DIVPS;
                } else if (in.type == ir::IrType::I64 ||
                           in.type == ir::IrType::U64) {
                    if (subop == 0) pop = MOp::PADDQ;
                    else if (subop == 1) pop = MOp::PSUBQ;
                    else return false; // mul/div i64 no packed en SSE2
                } else if (in.type == ir::IrType::I32 ||
                           in.type == ir::IrType::U32) {
                    if (subop == 0) pop = MOp::PADDD;
                    else if (subop == 1) pop = MOp::PSUBD;
                    else if (subop == 2) pop = MOp::PMULLD; // SSE4.1/AVX2
                    else return false; // div i32 no packed
                } else if (in.type == ir::IrType::I16 ||
                           in.type == ir::IrType::U16) {
                    if (subop == 0) pop = MOp::PADDW;
                    else if (subop == 1) pop = MOp::PSUBW;
                    else if (subop == 2) pop = MOp::PMULLW; // SSE2 word mul (low)
                    else return false; // div i16 no packed
                } else if (in.type == ir::IrType::I8 ||
                           in.type == ir::IrType::U8) {
                    if (subop == 0) pop = MOp::PADDB;
                    else if (subop == 1) pop = MOp::PSUBB;
                    else return false; // no hay mul/div byte packed en SSE2
                } else {
                    return false; // tipo no soportado
                }
                // ROBUSTEZ (clobber set formal): los scratch los DERIVAMOS del
                // TargetRegInfo (tri_sel.scratch[GP]/[FP]) en vez de hardcodear
                // R10/R11/XMM14/XMM15.  Estos registros estan RESERVADOS por el
                // allocator (no son asignables -> nunca contienen un valor vivo)
                // y son los MISMOS que el rewrite usa como scratch GP (scr0/scr1)
                // y FP (fscr0/fscr1).  Invariante de seguridad: el vector vive en
                // los 2 XMM scratch (fp0/fp1) a traves del `MOV gp0, c_ptr`
                // intermedio; ese MOV solo materializa un operando GP (puntero) ->
                // usa scratch GP, NUNCA toca el scratch FP -> el vector sobrevive.
                // Si el target NO reserva >=2 GP + >=2 FP scratch, bail (cola
                // escalar).  Asi un cambio de scratch del target no corrompe en
                // silencio: el VEC_BINOP sigue automaticamente al target.
                const auto &gpsc =
                    tri_sel.scratch[static_cast<size_t>(RegClass::GP)];
                const auto &fpsc =
                    tri_sel.scratch[static_cast<size_t>(RegClass::FP)];
                if (gpsc.size() < 2 || fpsc.size() < 2) return false;
                const MReg gp0 = static_cast<MReg>(gpsc[0]);
                const MReg gp1 = static_cast<MReg>(gpsc[1]);
                const MReg fp0 = static_cast<MReg>(fpsc[0]);
                const MReg fp1 = static_cast<MReg>(fpsc[1]);
                /* x0/x1 con ancho eff_w (16/32/64) -> el encoder elige
                 * SSE2/VX/EVEX.  Una pieza procesa eff_w bytes en el offset
                 * piece*eff_w; recargamos la base por pieza (solo 2 GP scratch)
                 * y le sumamos el offset (cuando n_pieces>1). */
                const uint8_t ew = static_cast<uint8_t>(eff_w);
                const MOperand x0 = MOperand::make_reg(fp0, ew);
                const MOperand x1 = MOperand::make_reg(fp1, ew);
                const MOperand r0 = MOperand::make_reg(gp0, 8);
                const MOperand r1 = MOperand::make_reg(gp1, 8);
                for (uint64_t pc = 0; pc < n_pieces; ++pc) {
                    // offset de la pieza dentro del chunk (0 cuando n_pieces=1,
                    // que es el unico caso que llega a EVEX -> sin disp).
                    const int32_t off = static_cast<int32_t>(pc * eff_w);
                    // load a -> x0  ([base+off])
                    O.push_back(MInstr::make_unary(MOp::MOV, r0,
                                                   vr(in.operands[1])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD, x0, MOperand::make_mem(gp0, off)));
                    // load b -> x1
                    O.push_back(MInstr::make_unary(MOp::MOV, r1,
                                                   vr(in.operands[2])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD, x1, MOperand::make_mem(gp1, off)));
                    O.push_back(MInstr::make_unary(pop, x0, x1)); // x0 OP= x1
                    // store -> dst
                    O.push_back(MInstr::make_unary(MOp::MOV, r0,
                                                   vr(in.operands[0])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD, MOperand::make_mem(gp0, off), x0));
                }
                break;
            }

            /* VEC_BINOP_S dst[i] = a[i] OP escalar (escalado/offset).  El escalar
             * (f64, vreg FP) se DIFUNDE a todos los lanes en x1 UNA vez
             * (MOVSD x1,scalar + UNPCKLPD/VBROADCASTSD); luego por chunk MOVUPD
             * x0,[a]; <op> x0,x1; MOVUPD [dst],x0.  Solo f64 por ahora. */
            case ir::IrOp::VEC_BINOP_S: {
                flush_pending();
                if (in.operands.size() != 3) return false;
                if (!fp_ok) return false;
                const uint64_t chunk_w = in.imm & 0xFF;
                const uint64_t subop = (in.imm >> 8) & 0xFF;
                if (chunk_w != 16 && chunk_w != 32 && chunk_w != 64)
                    return false;
                // op packed segun el tipo de elemento.  El escalar ya viene
                // REPLICADO a 64 bits (matcher) para los enteros -> un broadcast
                // de lane de 64 bits (UNPCKLPD/VBROADCASTSD) llena todos los
                // sub-lanes con el escalar.
                const bool is_fp = (in.type == ir::IrType::F64);
                MOp pop;
                if (in.type == ir::IrType::F64) {
                    pop = (subop == 0)   ? MOp::ADDPD
                          : (subop == 1) ? MOp::SUBPD
                          : (subop == 2) ? MOp::MULPD
                                         : MOp::DIVPD;
                } else if (in.type == ir::IrType::I64 ||
                           in.type == ir::IrType::U64) {
                    if (subop == 0) pop = MOp::PADDQ;
                    else if (subop == 1) pop = MOp::PSUBQ;
                    else return false;
                } else if (in.type == ir::IrType::I32 ||
                           in.type == ir::IrType::U32) {
                    if (subop == 0) pop = MOp::PADDD;
                    else if (subop == 1) pop = MOp::PSUBD;
                    else if (subop == 2) pop = MOp::PMULLD;
                    else return false;
                } else if (in.type == ir::IrType::I16 ||
                           in.type == ir::IrType::U16) {
                    if (subop == 0) pop = MOp::PADDW;
                    else if (subop == 1) pop = MOp::PSUBW;
                    else if (subop == 2) pop = MOp::PMULLW;
                    else return false;
                } else if (in.type == ir::IrType::I8 ||
                           in.type == ir::IrType::U8) {
                    if (subop == 0) pop = MOp::PADDB;
                    else if (subop == 1) pop = MOp::PSUBB;
                    else return false;
                } else {
                    return false;
                }
                (void)is_fp;
                // HOIST: el escalar ya esta DIFUNDIDO en XMM13 por un VEC_BCAST
                // del preheader (imm bit 16).  Asi el cuerpo del loop es VX PURO
                // (vmovupd ymm + vop ymm leyendo XMM13) -> ancho AVX/AVX512 sin
                // re-broadcast ni transicion AVX<->SSE.  XMM13 = acc0 reservado;
                // scalar-bcast y reduccion no coexisten en un matcher de 1 stmt.
                const bool hoisted = (in.imm >> 16) & 1;
                const uint64_t host_w = vec_host_w();
                // sin hoist (no deberia pasar desde el matcher actual): fallback
                // a SSE2 128b (legacy, sin transicion) con auto-broadcast.
                const uint64_t eff_w = hoisted
                                           ? (chunk_w < host_w ? chunk_w : host_w)
                                           : 16;
                const uint64_t n_pieces = chunk_w / eff_w;
                const auto &gpsc =
                    tri_sel.scratch[static_cast<size_t>(RegClass::GP)];
                const auto &fpsc =
                    tri_sel.scratch[static_cast<size_t>(RegClass::FP)];
                if (gpsc.empty() || fpsc.size() < 2) return false;
                const MReg gp0 = static_cast<MReg>(gpsc[0]);
                const MReg fp0 = static_cast<MReg>(fpsc[0]);
                const MReg fp1 = static_cast<MReg>(fpsc[1]);
                const uint8_t ew = static_cast<uint8_t>(eff_w);
                const MOperand x0 = MOperand::make_reg(fp0, ew);
                // escalar wide: hoisted -> XMM(13-sidx) (pre-difundido por su
                // VEC_BCAST; sidx=indice del escalar en imm bits 17-19, permite
                // multiples escalares en XMM10-13 sin colision); si no, fp1.
                const uint64_t sidx = (in.imm >> 17) & 0x7;
                const MReg scalreg =
                    hoisted
                        ? static_cast<MReg>(reg_id(MReg::XMM13) - sidx)
                        : fp1;
                const MOperand x1 = MOperand::make_reg(scalreg, ew);
                if (!hoisted) {
                    // difundir el escalar a fp1 (una vez): f64 via MOVSD; entero
                    // via MOVQ_GP_XMM del escalar replicado.  Solo SSE2 128b.
                    if (is_fp)
                        O.push_back(MInstr::make_unary(
                            MOp::MOVSD, MOperand::make_reg(fp1, 8),
                            vrt(in.operands[2])));
                    else
                        O.push_back(MInstr::make_unary(
                            MOp::MOVQ_GP_XMM, MOperand::make_reg(fp1, 16),
                            vr(in.operands[2])));
                    O.push_back(MInstr::make_unary(
                        MOp::UNPCKLPD, MOperand::make_reg(fp1, 16),
                        MOperand::make_reg(fp1, 16)));
                }
                for (uint64_t pc = 0; pc < n_pieces; ++pc) {
                    const int32_t off = static_cast<int32_t>(pc * eff_w);
                    O.push_back(MInstr::make_unary(MOp::MOV,
                                                   MOperand::make_reg(gp0, 8),
                                                   vr(in.operands[1])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD, x0, MOperand::make_mem(gp0, off)));
                    O.push_back(MInstr::make_unary(pop, x0, x1)); // x0 OP= esc
                    O.push_back(MInstr::make_unary(MOp::MOV,
                                                   MOperand::make_reg(gp0, 8),
                                                   vr(in.operands[0])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD, MOperand::make_mem(gp0, off), x0));
                }
                break;
            }

            /* VEC_BCAST: difunde el escalar (operands[0]) a TODOS los lanes de
             * XMM13 (reservado) UNA vez en el preheader.  El VEC_BINOP_S del
             * cuerpo lo reusa (hoist) sin re-broadcast.  Una sola transicion
             * AVX<->SSE aqui (fuera del loop) es despreciable. */
            case ir::IrOp::VEC_BCAST: {
                flush_pending();
                if (in.operands.empty()) return false;
                if (!fp_ok) return false;
                const uint64_t chunk_w = in.imm & 0xFF;
                if (chunk_w != 16 && chunk_w != 32 && chunk_w != 64)
                    return false;
                const bool is_fp = (in.type == ir::IrType::F64);
                const uint64_t host_w = vec_host_w();
                const uint64_t eff_w = (chunk_w < host_w) ? chunk_w : host_w;
                // reg destino del broadcast: XMM(13-idx), idx en imm bits 8-10
                // (permite multiples escalares en XMM10-13 sin colision).
                const uint64_t bcidx = (in.imm >> 8) & 0x7;
                const MReg B = static_cast<MReg>(reg_id(MReg::XMM13) - bcidx);
                // cargar el escalar al low de XMM13: f64 via MOVSD; entero via
                // MOVQ_GP_XMM del valor ya replicado a 64b.
                if (is_fp)
                    O.push_back(MInstr::make_unary(
                        MOp::MOVSD, MOperand::make_reg(B, 8),
                        vrt(in.operands[0])));
                else
                    O.push_back(MInstr::make_unary(
                        MOp::MOVQ_GP_XMM, MOperand::make_reg(B, 16),
                        vr(in.operands[0])));
                // difundir el lane de 64b a todo el ancho eff_w.
                if (eff_w <= 16)
                    O.push_back(MInstr::make_unary(
                        MOp::UNPCKLPD, MOperand::make_reg(B, 16),
                        MOperand::make_reg(B, 16)));
                else
                    O.push_back(MInstr::make_unary(
                        MOp::VBROADCASTSD, MOperand::make_reg(B, eff_w),
                        MOperand::make_reg(B, 16)));
                break;
            }

            /* VEC_FMA fusionado (1 redondeo).  Dos formas segun #operandos:
             *  - 3 ops {acc,a,b}: reduccion acc[i] += a[i]*b[i] (acc es a la vez
             *    sumando y destino).
             *  - 4 ops {c,d,a,b}: element-wise c[i] = a[i]*b[i] + d[i] (el
             *    sumando d es un array DISTINTO del destino c).
             * Por chunk: MOVUPD x0,[sumando]; MOVUPD x1,[a]; VFMADD231 x0,x1,[b]
             * (x0 = a*b + sumando); MOVUPD [dst],x0.  Mismo chunk/descompone y
             * scratch (gp0/gp1, fp0/fp1) que VEC_BINOP; b de memoria -> 2 XMM. */
            case ir::IrOp::VEC_FMA: {
                flush_pending();
                if (in.operands.size() != 3 && in.operands.size() != 4)
                    return false;
                const bool fma3 = (in.operands.size() == 4); // element-wise
                const int o_dst = 0;                  // destino (c o acc)
                const int o_add = fma3 ? 1 : 0;       // sumando (d o acc)
                const int o_a = fma3 ? 2 : 1;         // multiplicando a
                const int o_b = fma3 ? 3 : 2;         // multiplicando b (memoria)
                const uint64_t chunk_w = in.imm & 0xFF;
                if (chunk_w != 16 && chunk_w != 32 && chunk_w != 64)
                    return false;
                if (!fp_ok) return false;
                MOp fma;
                if (in.type == ir::IrType::F64) fma = MOp::VFMADD231PD;
                else if (in.type == ir::IrType::F32) fma = MOp::VFMADD231PS;
                else return false;
                const uint64_t host_w =
                    vec_host_w();
                // FMA requiere AVX (>=256); con host SSE2 solo, bail (el
                // interp/loop escalar lo hace).  256/512 -> ymm/zmm.
                uint64_t emit_w = host_w;
                if (emit_w < 32) return false; // sin AVX no hay VFMADD
                const uint64_t eff_w = (chunk_w < emit_w) ? chunk_w : emit_w;
                if (eff_w < 32) return false;
                const uint64_t n_pieces = chunk_w / eff_w;
                const auto &gpsc =
                    tri_sel.scratch[static_cast<size_t>(RegClass::GP)];
                const auto &fpsc =
                    tri_sel.scratch[static_cast<size_t>(RegClass::FP)];
                if (gpsc.size() < 2 || fpsc.size() < 2) return false;
                const MReg gp0 = static_cast<MReg>(gpsc[0]);
                const MReg gp1 = static_cast<MReg>(gpsc[1]);
                const MReg fp0 = static_cast<MReg>(fpsc[0]);
                const MReg fp1 = static_cast<MReg>(fpsc[1]);
                const uint8_t ew = static_cast<uint8_t>(eff_w);
                const MOperand x0 = MOperand::make_reg(fp0, ew); // acc
                const MOperand x1 = MOperand::make_reg(fp1, ew); // a
                const MOperand r0 = MOperand::make_reg(gp0, 8);
                const MOperand r1 = MOperand::make_reg(gp1, 8);
                for (uint64_t pc = 0; pc < n_pieces; ++pc) {
                    const int32_t off = static_cast<int32_t>(pc * eff_w);
                    // x0 = sumando[off] (acc en reduccion, d en element-wise)
                    O.push_back(MInstr::make_unary(MOp::MOV, r0,
                                                   vr(in.operands[o_add])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD, x0, MOperand::make_mem(gp0, off)));
                    // x1 = a[off]
                    O.push_back(MInstr::make_unary(MOp::MOV, r1,
                                                   vr(in.operands[o_a])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD, x1, MOperand::make_mem(gp1, off)));
                    // x0 = x1 * [b+off] + x0
                    O.push_back(MInstr::make_unary(MOp::MOV, r0,
                                                   vr(in.operands[o_b])));
                    O.push_back(MInstr::make_binary(
                        fma, x0, x1, MOperand::make_mem(gp0, off)));
                    // dst[off] = x0 (c en element-wise, acc en reduccion)
                    O.push_back(MInstr::make_unary(MOp::MOV, r0,
                                                   vr(in.operands[o_dst])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD, MOperand::make_mem(gp0, off), x0));
                }
                break;
            }

            /* VEC_ACC_* : acumulador vectorial REGISTER-RESIDENT para
             * reducciones/dot-products.  El acc vive en XMM13 (DEDICADO,
             * reservado en target_reginfo) a TRAVES del bucle -> sin round-trip
             * a memoria por iteracion.  Solo 1 registro -> sin descomposicion:
             * bail si el chunk excede el ancho del host (cae a interp).  La
             * scratch FP del a/b es XMM14 (fp0). */
            case ir::IrOp::VEC_ACC_ZERO:
            case ir::IrOp::VEC_ACC_ADD:
            case ir::IrOp::VEC_ACC_FMA:
            case ir::IrOp::VEC_ACC_COMBINE:
            case ir::IrOp::VEC_ACC_STORE: {
                flush_pending();
                if (!fp_ok) return false;
                const uint64_t chunk_w = in.imm & 0xFF;
                if (chunk_w != 16 && chunk_w != 32 && chunk_w != 64)
                    return false;
                const uint64_t host_w =
                    vec_host_w();
                if (chunk_w > host_w) return false; // acc de 1 reg, sin split
                const bool is_f32 = (in.type == ir::IrType::F32);
                const bool is_f64 = (in.type == ir::IrType::F64);
                const bool is_i64 = (in.type == ir::IrType::I64 ||
                                     in.type == ir::IrType::U64);
                const bool is_i32 = (in.type == ir::IrType::I32 ||
                                     in.type == ir::IrType::U32);
                if (!is_f32 && !is_f64 && !is_i64 && !is_i32) return false;
                // FMA solo float (no hay multiplica-acumula entero packed).
                if (in.op == ir::IrOp::VEC_ACC_FMA && !is_f32 && !is_f64)
                    return false;
                const auto &gpsc =
                    tri_sel.scratch[static_cast<size_t>(RegClass::GP)];
                const auto &fpsc =
                    tri_sel.scratch[static_cast<size_t>(RegClass::FP)];
                if (gpsc.empty() || fpsc.empty()) return false;
                const MReg gp0 = static_cast<MReg>(gpsc[0]);
                const MReg gp1 =
                    (gpsc.size() >= 2) ? static_cast<MReg>(gpsc[1]) : gp0;
                const uint8_t ew = static_cast<uint8_t>(chunk_w);
                // acc_idx (bits 8-11) -> XMM(13-idx): acc0=XMM13 .. acc3=XMM10.
                const uint8_t acc_idx = (in.imm >> 8) & 0xF;
                const uint8_t src_idx = (in.imm >> 12) & 0xF; // COMBINE
                if (acc_idx > 3 || src_idx > 3) return false; // solo 4 reservados
                // disp de array (bits 16-31): displacement constante de la
                // pieza del unroll para ADD/FMA -> `movupd disp(base)` en vez de
                // recalcular el puntero.  ZERO/COMBINE/STORE lo ignoran.
                const int32_t arr_disp =
                    static_cast<int32_t>((in.imm >> 16) & 0xFFFF);
                const MReg ACC =
                    static_cast<MReg>(reg_id(MReg::XMM13) - acc_idx);
                const MReg SCR = static_cast<MReg>(fpsc[0]); // XMM14 scratch a/b
                const MOperand xacc = MOperand::make_reg(ACC, ew);
                const MOperand xscr = MOperand::make_reg(SCR, ew);
                if (in.op == ir::IrOp::VEC_ACC_ZERO) {
                    // acc = 0.  XORPD (no XORPS) para ambos f32/f64: el zeroing
                    // es agnostico al tipo y XORPD va al packed-arith case que
                    // SI maneja el ancho (16/32/64); XORPS(102) es scalar 128b.
                    O.push_back(MInstr::make_unary(MOp::XORPD, xacc, xacc));
                } else if (in.op == ir::IrOp::VEC_ACC_ADD) {
                    // acc += a[chunk]:  MOVUPD scr,[a]; <op> acc,scr.
                    // float -> ADDP{D,S}; entero -> PADDQ (i64) / PADDD (i32).
                    const MOp aop = is_f32   ? MOp::ADDPS
                                    : is_f64 ? MOp::ADDPD
                                    : is_i64 ? MOp::PADDQ
                                             : MOp::PADDD;
                    O.push_back(MInstr::make_unary(MOp::MOV,
                                                   MOperand::make_reg(gp0, 8),
                                                   vr(in.operands[1])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD, xscr, MOperand::make_mem(gp0, arr_disp)));
                    O.push_back(MInstr::make_unary(aop, xacc, xscr));
                } else if (in.op == ir::IrOp::VEC_ACC_FMA) {
                    // acc += a*b:  MOVUPD scr,[a]; VFMADD231 acc,scr,[b].
                    O.push_back(MInstr::make_unary(MOp::MOV,
                                                   MOperand::make_reg(gp0, 8),
                                                   vr(in.operands[1])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD, xscr, MOperand::make_mem(gp0, arr_disp)));
                    O.push_back(MInstr::make_unary(MOp::MOV,
                                                   MOperand::make_reg(gp1, 8),
                                                   vr(in.operands[2])));
                    O.push_back(MInstr::make_binary(
                        is_f32 ? MOp::VFMADD231PS : MOp::VFMADD231PD, xacc,
                        xscr, MOperand::make_mem(gp1, arr_disp)));
                } else if (in.op == ir::IrOp::VEC_ACC_COMBINE) {
                    // acc[dst] += acc[src]  (reg-reg, sin memoria).
                    const MReg SRC =
                        static_cast<MReg>(reg_id(MReg::XMM13) - src_idx);
                    const MOp aop = is_f32   ? MOp::ADDPS
                                    : is_f64 ? MOp::ADDPD
                                    : is_i64 ? MOp::PADDQ
                                             : MOp::PADDD;
                    O.push_back(MInstr::make_unary(
                        aop, xacc, MOperand::make_reg(SRC, ew)));
                } else { // VEC_ACC_STORE: slot[idx] = acc[idx].
                    O.push_back(MInstr::make_unary(MOp::MOV,
                                                   MOperand::make_reg(gp0, 8),
                                                   vr(in.operands[0])));
                    O.push_back(MInstr::make_unary(
                        MOp::MOVUPD,
                        MOperand::make_mem(
                            gp0, static_cast<int32_t>(acc_idx * chunk_w)),
                        xacc));
                }
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
                if (vx::g_asm_backend == nullptr) {
                    vreg_dbg(fn.name.c_str(), "inline-asm(no-backend)");
                    return false;
                }
                // si hay operandos `reg` AUTO, el cuerpo lleva $N y el
                // fisico de cada uno lo elige el RA -> DIFERIR el ensamblado a
                // post-regalloc (regalloc_rewrite).  @c ar queda vacio (los
                // sym_refs/labels/bytes se rellenan al ensamblar en el rewrite).
                bool asm_has_auto = false;
                for (const ir::AsmRegBinding &b : fn.asm_reg_bindings)
                    if (b.reg_auto) { asm_has_auto = true; break; }
                // El inline-asm de @Naked/asm{} se ensambla en el modo del
                // TARGET (no del host): x86-32 -> KS_MODE_32 (si no, `jmp ecx`
                // y demas codificaciones de 32 bits fallan en KS_MODE_64).
                vx::AsmAssembleResult ar;
                if (!asm_has_auto) {
                    ar = vx::g_asm_backend->assemble(
                        in.func_name,
                        mode32 ? vx::AsmArch::X86_32 : vx::AsmArch::X86_64);
                    if (!ar.ok || ar.bytes.empty()) {
                        vreg_dbg(fn.name.c_str(), "inline-asm(assemble-fail)");
                        return false;
                    }
                }
                AsmBlob blob;
                // Phase AS inc.6: simbolos PROPIOS referenciados desde el asm
                // (`jmp [global]`, `mov rax, fn`).  El backend ya localizo el
                // campo (offset relativo al blob) + tipo; el encoder los reubica
                // y emite los MReloc.  Cero coste si el asm no usa simbolos.
                for (const auto &sr : ar.sym_refs) {
                    AsmBlob::AsmSymRef br;
                    br.offset = sr.offset;
                    br.size = sr.size;
                    br.pcrel_trailing = sr.pcrel_trailing;
                    br.symbol = sr.symbol;
                    using SK = vx::AsmAssembleResult::SymRefKind;
                    using BK = AsmBlob::AsmSymRefKind;
                    switch (sr.kind) {
                    case SK::BranchRel32: br.kind = BK::BranchRel32; break;
                    case SK::DataRel32: br.kind = BK::DataRel32; break;
                    case SK::Abs64: br.kind = BK::Abs64; break;
                    case SK::Abs32: br.kind = BK::Abs32; break;
                    }
                    blob.sym_refs.push_back(std::move(br));
                }
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
                            const std::string c = vx::asm_canonical_reg(cn);
                            const int phys = canon_gp_to_mreg(c);
                            if (phys >= 0) {
                                blob.clobbers.push_back(
                                    static_cast<uint8_t>(phys));
                            } else if (c == "rbx") {
                                save_rbx = true;
                            } else if (c == "rbp") {
                                save_rbp = true;
                            } else if (c == "rsp") {
                                // Un asm que reasigna rsp (stack switch para
                                // corrutinas/fibras) se compila nativamente en
                                // ambos casos: NO cae al interp.  En @Naked el
                                // usuario es dueno de la pila.  En una funcion
                                // NORMAL su epilogue gestiona rsp; el frontend ya
                                // avisa (warning) de que un cambio de pila
                                // persistente exige @Naked.  rsp no se registra
                                // como clobber (no es asignable por el RA); el
                                // asm lo maneja verbatim.
                            }
                        }
                    }
                }
                // blob DIFERIDO -> plantilla ($N) + descriptor por
                // operando `reg` auto (su alloca vreg; el rewrite pone
                // ra.reg_of(vreg) en $ph_index).  Los operandos concretos ya
                // quedaron horneados en la plantilla por el ph_subst del lowering.
                if (asm_has_auto) {
                    blob.deferred = true;
                    blob.deferred_isa = 0; // instr_db::Isa::X86
                    blob.deferred_tmpl = in.func_name;
                    int maxph = -1;
                    for (const ir::AsmRegBinding &b : fn.asm_reg_bindings)
                        if (b.reg_auto && b.ph_index > maxph) maxph = b.ph_index;
                    blob.deferred_ops.resize(maxph + 1);
                    for (const ir::AsmRegBinding &b : fn.asm_reg_bindings) {
                        if (!b.reg_auto || b.ph_index < 0) continue;
                        AsmBlob::DeferredOp &d =
                            blob.deferred_ops[b.ph_index];
                        d.vreg = static_cast<uint32_t>(b.alloca_value);
                        d.fixed_phys = -1; // lo elige el RA
                        d.width = static_cast<uint16_t>(
                            ir_type_bytes(b.type) * 8);
                        d.regclass = 0; // GP
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

            /* ASM_MICRO: una instruccion asm OPACA liftada (ver AsmMicro).
             * Su plantilla se ENSAMBLA (via g_asm_backend, con cache) y se
             * emite como INLINE_ASM_RAW (el encoder la apendea verbatim); sus
             * efectos (barrera de memoria / flags) los da la DB (campo eff).
             *
             * Este incremento cubre el caso SIN operandos de registro
             * (mfence/pause/lfence/sfence/cpuid-sin-regs...): la plantilla ES
             * el texto final, sin placeholders que rellenar.  El caso con
             * operandos (substitucion de $N por el reg fisico + pinning en el
             * regalloc) llega en un incremento posterior; el lifter hoy solo
             * produce ASM_MICRO sin operandos, asi que ins/outs no-vacios ->
             * fallback (no deberia ocurrir). */
            case ir::IrOp::ASM_MICRO: {
                flush_pending();
                if (vx::g_asm_backend == nullptr) {
                    vreg_dbg(fn.name.c_str(), "asm_micro(no-backend)");
                    return false;
                }
                if (in.imm >= fn.asm_micros.size()) return false;
                const ir::AsmMicro &am = fn.asm_micros[in.imm];
                // sustituir $0,$1,... por el nombre del registro FiSICO
                // FIJO de cada operando ANTES de ensamblar.  Sin operandos, la
                // plantilla no tiene $N y queda verbatim (caso mfence/etc).
                std::string nasm = am.tmpl;
                if (!am.operands.empty() &&
                    !vx::asm_micro_subst_phys(am, nasm)) {
                    // Operando no fisico (SSA/RA) o clase no soportada.
                    vreg_dbg(fn.name.c_str(), "asm_micro(operando-no-fisico)");
                    return false;
                }
                vx::AsmAssembleResult ar = vx::g_asm_backend->assemble(
                    nasm, mode32 ? vx::AsmArch::X86_32 : vx::AsmArch::X86_64);
                if (!ar.ok || ar.bytes.empty()) {
                    vreg_dbg(fn.name.c_str(), "asm_micro(assemble-fail)");
                    return false;
                }
                AsmBlob blob;
                blob.bytes = std::move(ar.bytes);
                // Efectos de la DB: bit0 mem, bit3 barrera -> clobber de
                // memoria; bit2 escribe flags.
                blob.clobbers_mem = (am.eff & 0x9) != 0;
                blob.clobbers_flags = (am.eff & 0x4) != 0;
                const uint32_t bidx = out.intern_asm_blob(std::move(blob));
                O.push_back(MInstr::make_inline_asm_raw(bidx));
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
                if (in.operands.size() != 1) return false;
                if (in.dst == ir::IR_NO_VALUE) break; // lookup sin uso: no-op
                if (!vm) {
                    // HOST_LEAF (AOT native, sin handle table): el "handle"
                    // almacenado ES el host_ptr crudo (objetos = ptr de
                    // calloc/malloc, no GcHandle) -> passthrough MOV dst,src.
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                                   vr(in.operands[0])));
                    break;
                }
                if (ent.gc_deref == 0) {
                    vreg_dbg(fn.name.c_str(), "gc_deref(no-addr)");
                    return false;
                }

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

            /* GCWB_IR: write-barrier generacional old->young.
             *   vrt_gc_write_barrier(proc, container_handle) -> void.
             * write_barrier() filtra por generacion (skip si el contenedor es
             * YOUNG) y solo inserta en el remembered_set (host malloc, NO dispara
             * GC) -> no es safepoint.  En HOST_LEAF (AOT native, sin nursery /
             * sin runtime) es NO-OP: el GC del AOT usa alloc_pinned (todo OldGen)
             * y el major escanea preciso via field-maps -> el barrier no aporta;
             * se omite.  Marshalling 1-arg (proc=A0, handle=A1). */
            case ir::IrOp::GCWB_IR: {
                if (!vm || ent.gc_write_barrier == 0 || in.operands.size() != 1)
                    break; // AOT / sin runtime: no-op
                flush_pending();
#if defined(_WIN32)
                const MReg wb0 = MReg::RCX, wb1 = MReg::RDX;
#else
                const MReg wb0 = MReg::RDI, wb1 = MReg::RSI;
#endif
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(wb1, 8),
                                               vr(in.operands[0]))); // handle
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(wb0, 8),
                    MOperand::make_reg(MReg::RBX, 8))); // proc
                O.push_back(MInstr::make_call_abs(
                    out.intern_imm64(ent.gc_write_barrier)));
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
                if (in.op == ir::IrOp::GC_HANDLE_FOR_PTR && !vm) {
                    // HOST_LEAF (AOT native, sin handle table): el GcHandle de un
                    // ptr ES el propio host_ptr (objetos = ptr crudo) ->
                    // passthrough MOV dst, src.
                    if (in.dst != ir::IR_NO_VALUE && in.operands.size() == 1)
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                                       vr(in.operands[0])));
                    break;
                }
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
                // BUG-2 fix: buffer HOST (ALLOCA promovido a heap host que
                // fluye a un CALLN de stringify) -> vrt_str_make_h que lee
                // memoria host directa (paridad con el opcode strmake_h del
                // interp).  Sin esto, el path vreg caia a slots que emitia
                // `strmake` (VM mem) sobre un buffer host -> bytes NUL en los
                // fragmentos interpolados de `return "${expr}..."`.
                const bool sm_buf_host =
                    in.operands[0] < fn.values.size() &&
                    fn.values[in.operands[0]].is_host_ptr;
                const uint64_t sm_entry =
                    sm_buf_host ? ent.str_make_h : ent.str_make;
                if (sm_buf_host && ent.str_make_h == 0) {
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
                    MInstr::make_call_abs(out.intern_imm64(sm_entry)));
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

            /* CALL intra-modulo a otra funcion Vesta (VM_ABI): los args
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
                /* 1. Stores de args a proc->registers.regs[i+1]
                 *    (float-aware via store_vm_arg). */
                for (size_t i = 0; i < in.operands.size(); ++i)
                    store_vm_arg(O, static_cast<int>(i) + 1, in.operands[i]);
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
                /* FN.3 pieza 2: CALLIND en VM_ABI (JIT).  En runtime el
                 * func_ptr puede ser (a) un puntero nativo naked (HOST_LEAF,
                 * de `(cfn)fn`), (b) un jit_code VM_ABI (de `&fn` con la pieza
                 * 1) o (c) una VA de bytecode (fn no compilada aun).  Replicamos
                 * EXACTAMENTE la semantica del interp `exec_instr_callvmr` via el
                 * helper de runtime `vrt_callind`: marshalling de los args a
                 * proc->registers.regs[1..N] + regs[15]=nargs (la MISMA
                 * convencion que el bytecode CALLIND emite), luego CALL nativo a
                 * vrt_callind(proc, func_ptr) que dispatcha por rango (naked ->
                 * ABI host via invoke_native_unchecked; jit/VA -> VM_ABI, con
                 * compile-on-demand para la VA).  El resultado queda en regs[0].
                 * El caller es JIT y el callee corre nativo -> ni caller ni
                 * callee caen a interp (para el corpus la VA compila siempre). */
                if (ent.callind == 0 || in.func_ptr == ir::IR_NO_VALUE) {
                    vreg_dbg(fn.name.c_str(), "callind(no-entry)");
                    return false;
                }
                /* 1. Args -> proc->registers.regs[i+1] (float-aware). */
                for (size_t i = 0; i < in.operands.size(); ++i)
                    store_vm_arg(O, static_cast<int>(i) + 1, in.operands[i]);
                /* 2. nargs -> regs[15] (argc para el bridge naked host-ABI). */
                O.push_back(MInstr::make_unary(
                    MOp::MOV, vm_reg_mem(15),
                    MOperand::make_imm32(
                        static_cast<int32_t>(in.operands.size()))));
                /* 3. func_ptr -> arg1 (via R10 temporal, como SMARTPTR_FREE,
                 *    para no chocar con el orden de asignacion de arg-regs);
                 *    proc (RBX) -> arg0. */
#if defined(_WIN32)
                const MReg ci_a0 = MReg::RCX, ci_a1 = MReg::RDX;
#else
                const MReg ci_a0 = MReg::RDI, ci_a1 = MReg::RSI;
#endif
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::R10, 8),
                    vr(in.func_ptr)));
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(ci_a1, 8),
                    MOperand::make_reg(MReg::R10, 8)));
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(ci_a0, 8),
                    MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.callind)));
                /* 4. Resultado desde regs[0]. */
                if (in.dst != ir::IR_NO_VALUE)
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                                   vm_reg_mem(0)));
                break;
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
                 *    que CALL; los args son vregs del frame actual;
                 *    float-aware via store_vm_arg). */
                for (size_t i = 0; i < in.operands.size(); ++i)
                    store_vm_arg(O, static_cast<int>(i) + 1, in.operands[i]);
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
                 *    (operands[0]=obj=this -> regs[1], args -> regs[2..];
                 *    float-aware via store_vm_arg). */
                for (size_t i = 0; i < in.operands.size(); ++i)
                    store_vm_arg(O, static_cast<int>(i) + 1, in.operands[i]);
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

            /* CALLM: dispatch via MethodInfo* ya resuelto (operands[1]).  No
             * hay vtable walk (a diferencia de CALLVIRT): inline directo del
             * method->jit_code (cero runtime en el fast path; vrt_callm ~150ns
             * solo en el frio).  operands[0]=obj, operands[1]=method,
             * operands[2..]=args.  Espejo del inline de slots (selector.cpp). */
            case ir::IrOp::CALLM: {
                flush_pending();
                if (!vm || ent.callm == 0) {
                    vreg_dbg(fn.name.c_str(), "callm(no-vm/no-addr)");
                    return false;
                }
                if (in.operands.size() < 2) {
                    vreg_dbg(fn.name.c_str(), "callm-shape");
                    return false;
                }
                const ir::IrValueId obj = in.operands[0];
                const ir::IrValueId method = in.operands[1];
                const size_t nargs =
                    in.operands.size() > 2 ? in.operands.size() - 2 : 0;
                /* Stage: regs[1]=obj, regs[2..]=args, regs[15]=nargs+1. */
                O.push_back(
                    MInstr::make_unary(MOp::MOV, vm_reg_mem(1), vr(obj)));
                for (size_t a = 0; a < nargs; ++a)
                    store_vm_arg(O, static_cast<int>(a) + 2,
                                 in.operands[a + 2]);
                O.push_back(MInstr::make_unary(
                    MOp::MOV, vm_reg_mem(15),
                    MOperand::make_imm32(static_cast<int32_t>(nargs + 1))));
#if defined(_WIN32)
                const MReg cm_pr = MReg::RCX, cm_obj = MReg::RDX,
                           cm_m = MReg::R8;
#else
                const MReg cm_pr = MReg::RDI, cm_obj = MReg::RSI,
                           cm_m = MReg::RDX;
#endif
                auto cm_load_field = [&](ir::IrValueId base,
                                         int32_t off) -> ir::IrValueId {
                    const ir::IrValueId d = new_tmp();
                    if (off == 0) {
                        O.push_back(
                            MInstr::make_load(vr(d), vr(base), 8, false));
                    } else {
                        const ir::IrValueId addr = new_tmp();
                        O.push_back(
                            MInstr::make_unary(MOp::MOV, vr(addr), vr(base)));
                        O.push_back(MInstr::make_binary(
                            MOp::ADD, vr(addr), vr(addr),
                            MOperand::make_imm32(off)));
                        O.push_back(
                            MInstr::make_load(vr(d), vr(addr), 8, false));
                    }
                    return d;
                };
                const MLabelId Lcm_fb = out.new_label();
                const MLabelId Lcm_done = out.new_label();
                /* INLINE: method no-null; sin advice; jit_code presente. */
                O.push_back(mk_test(method, method));
                O.push_back(MInstr::make_jcc(MCond::E, Lcm_fb));
                const ir::IrValueId cm_adv = cm_load_field(
                    method, VESTA_METHODINFO_ADVICE_CHAIN_OFFSET);
                O.push_back(mk_test(cm_adv, cm_adv));
                O.push_back(MInstr::make_jcc(MCond::NE, Lcm_fb));
                const ir::IrValueId cm_code =
                    cm_load_field(method, VESTA_METHODINFO_JIT_CODE_OFFSET);
                O.push_back(mk_test(cm_code, cm_code));
                O.push_back(MInstr::make_jcc(MCond::E, Lcm_fb));
                /* FAST: R10=code (scratch, no colisiona con pr_reg=rbx mov);
                 * proc en arg0; call indirecto R10. */
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::R10, 8), vr(cm_code)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(cm_pr, 8),
                                               MOperand::make_reg(MReg::RBX, 8)));
                {
                    MInstr ic;
                    ic.op = MOp::CALL;
                    ic.src1 = MOperand::make_reg(MReg::R10, 8);
                    O.push_back(ic);
                }
                O.push_back(MInstr::make_jmp(Lcm_done));
                /* FALLBACK: vrt_callm(proc, obj, method).  R10/R11 temporales
                 * para no clobber-ar arg regs antes de leerlos. */
                O.push_back(MInstr::make_label_def(Lcm_fb));
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::R10, 8), vr(obj)));
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::R11, 8), vr(method)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(cm_obj, 8),
                                               MOperand::make_reg(MReg::R10, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(cm_m, 8),
                                               MOperand::make_reg(MReg::R11, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(cm_pr, 8),
                                               MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(MInstr::make_call_abs(out.intern_imm64(ent.callm)));
                O.push_back(MInstr::make_label_def(Lcm_done));
                if (in.dst != ir::IR_NO_VALUE)
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                                   vm_reg_mem(0)));
                break;
            }

            /* CALLITF: dispatch de interfaz via itable.  operands[0]=obj,
             * operands[1]=params_ptr (iface+metodo, lo lee el runtime),
             * operands[2..]=args.  El itable scan es complejo (IC slot) ->
             * version simple: marshalling + vrt_callitf(proc, obj, params, 0)
             * (ic_slot=0 -> sin cache, resuelve cada vez; correcto).  El
             * inline-IC es optimizacion futura.  Retira de slots. */
            case ir::IrOp::CALLITF: {
                flush_pending();
                if (!vm || ent.callitf == 0) {
                    vreg_dbg(fn.name.c_str(), "callitf(no-vm/no-addr)");
                    return false;
                }
                if (in.operands.size() < 2) {
                    vreg_dbg(fn.name.c_str(), "callitf-shape");
                    return false;
                }
                const ir::IrValueId ci_obj = in.operands[0];
                const ir::IrValueId ci_params = in.operands[1];
                const size_t ci_nargs =
                    in.operands.size() > 2 ? in.operands.size() - 2 : 0;
                /* Stage: regs[1]=obj, regs[2..]=args, regs[15]=nargs+1. */
                O.push_back(
                    MInstr::make_unary(MOp::MOV, vm_reg_mem(1), vr(ci_obj)));
                for (size_t a = 0; a < ci_nargs; ++a)
                    store_vm_arg(O, static_cast<int>(a) + 2,
                                 in.operands[a + 2]);
                O.push_back(MInstr::make_unary(
                    MOp::MOV, vm_reg_mem(15),
                    MOperand::make_imm32(static_cast<int32_t>(ci_nargs + 1))));
                /* vrt_callitf(proc, obj, params, 0).  R10/R11 temporales. */
#if defined(_WIN32)
                const MReg cif_pr = MReg::RCX, cif_obj = MReg::RDX,
                           cif_par = MReg::R8, cif_ic = MReg::R9;
#else
                const MReg cif_pr = MReg::RDI, cif_obj = MReg::RSI,
                           cif_par = MReg::RDX, cif_ic = MReg::RCX;
#endif
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::R10, 8), vr(ci_obj)));
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::R11, 8), vr(ci_params)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(cif_obj, 8),
                                               MOperand::make_reg(MReg::R10, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(cif_par, 8),
                                               MOperand::make_reg(MReg::R11, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(cif_ic, 8),
                                               MOperand::make_imm32(0)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(cif_pr, 8),
                                               MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(MInstr::make_call_abs(out.intern_imm64(ent.callitf)));
                if (in.dst != ir::IR_NO_VALUE)
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                                   vm_reg_mem(0)));
                break;
            }

            /* CALLSUPER: super.metodo() -- dispatch a cls->vtable[idx] donde
             * cls es la SUPER class (operands[0], resuelta por findclass), NO
             * obj->class_ptr.  operands[1]=this, operands[2..]=args.  Resuelve
             * el method del super vtable (2 loads) y delega en vrt_callm(proc,
             * this, method) -> reusa el entry (method ya resuelto = el del
             * super, sin re-virtualizar).  Sin RAW_ASM. */
            case ir::IrOp::CALLSUPER: {
                flush_pending();
                if (!vm || ent.callm == 0) {
                    vreg_dbg(fn.name.c_str(), "callsuper(no-vm/no-addr)");
                    return false;
                }
                if (in.operands.size() < 2) {
                    vreg_dbg(fn.name.c_str(), "callsuper-shape");
                    return false;
                }
                const ir::IrValueId su_cls = in.operands[0];
                const ir::IrValueId su_this = in.operands[1];
                const uint32_t su_idx = static_cast<uint32_t>(in.imm);
                const size_t su_nargs =
                    in.operands.size() > 2 ? in.operands.size() - 2 : 0;
                /* Stage: regs[1]=this, regs[2..]=args, regs[15]=nargs+1. */
                O.push_back(
                    MInstr::make_unary(MOp::MOV, vm_reg_mem(1), vr(su_this)));
                for (size_t a = 0; a < su_nargs; ++a)
                    store_vm_arg(O, static_cast<int>(a) + 2,
                                 in.operands[a + 2]);
                O.push_back(MInstr::make_unary(
                    MOp::MOV, vm_reg_mem(15),
                    MOperand::make_imm32(static_cast<int32_t>(su_nargs + 1))));
                /* Resolver method = cls->vtable[idx] (2 loads).  cls es valido
                 * (super class resuelta); sin null-check defensivo. */
                auto su_load_field = [&](ir::IrValueId base,
                                         int32_t off) -> ir::IrValueId {
                    const ir::IrValueId d = new_tmp();
                    if (off == 0) {
                        O.push_back(
                            MInstr::make_load(vr(d), vr(base), 8, false));
                    } else {
                        const ir::IrValueId addr = new_tmp();
                        O.push_back(
                            MInstr::make_unary(MOp::MOV, vr(addr), vr(base)));
                        O.push_back(MInstr::make_binary(
                            MOp::ADD, vr(addr), vr(addr),
                            MOperand::make_imm32(off)));
                        O.push_back(
                            MInstr::make_load(vr(d), vr(addr), 8, false));
                    }
                    return d;
                };
                const ir::IrValueId su_vtbl =
                    su_load_field(su_cls, VESTA_CLASSINFO_VTABLE_OFFSET);
                const ir::IrValueId su_method =
                    su_load_field(su_vtbl, static_cast<int32_t>(su_idx * 8u));
                /* vrt_callm(proc, this, method).  R10/R11 temporales. */
#if defined(_WIN32)
                const MReg su_pr = MReg::RCX, su_obj = MReg::RDX,
                           su_m = MReg::R8;
#else
                const MReg su_pr = MReg::RDI, su_obj = MReg::RSI,
                           su_m = MReg::RDX;
#endif
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::R10, 8), vr(su_this)));
                O.push_back(MInstr::make_unary(
                    MOp::MOV, MOperand::make_reg(MReg::R11, 8), vr(su_method)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(su_obj, 8),
                                               MOperand::make_reg(MReg::R10, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(su_m, 8),
                                               MOperand::make_reg(MReg::R11, 8)));
                O.push_back(MInstr::make_unary(MOp::MOV,
                                               MOperand::make_reg(su_pr, 8),
                                               MOperand::make_reg(MReg::RBX, 8)));
                O.push_back(MInstr::make_call_abs(out.intern_imm64(ent.callm)));
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
                /* 1. Stores de args (operands[1..]) a regs[1..N]
                 *    (float-aware via store_vm_arg). */
                for (size_t i = 0; i < nargs; ++i)
                    store_vm_arg(O, static_cast<int>(i) + 1,
                                 in.operands[i + 1]);
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
                    /* Args ILIMITADOS (AOT): indice por clase; GP overflow
                     * -> pila (el rewrite lo coloca).  FP overflow -> bail. */
                    {
                        (void)host_leaf_nmax;
                        const size_t hl_fmax =
                            fp_ok ? tri_sel
                                        .arg_regs[static_cast<size_t>(
                                            RegClass::FP)]
                                        .size()
                                  : 0;
                        size_t hl_gi = 0, hl_fi = 0;
                        bool hl_fp_ovf = false;
                        std::vector<MInstr> hl_args;
                        for (size_t a = 0; a < nargs; ++a) {
                            const ir::IrValueId av = in.operands[a];
                            const bool is_f =
                                fp_ok && av < fn.values.size() &&
                                ir_type_is_float(fn.values[av].type);
                            if (is_f) {
                                if (hl_fi >= hl_fmax) {
                                    hl_fp_ovf = true;
                                    break;
                                }
                                hl_args.push_back(MInstr::make_arg(
                                    static_cast<uint8_t>(hl_fi), vrt(av)));
                                ++hl_fi;
                            } else {
                                hl_args.push_back(MInstr::make_arg(
                                    static_cast<uint8_t>(hl_gi), vr(av)));
                                ++hl_gi;
                            }
                        }
                        if (hl_fp_ovf) {
                            vreg_dbg(fn.name.c_str(), "calln(host-leaf-fp)");
                            return false;
                        }
                        for (auto &mi : hl_args) O.push_back(mi);
                    }
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
                /* Args ILIMITADOS (JIT/AOT): indice POR CLASE; los GP que
                 * no caben en arg_regs van por PILA (el rewrite los coloca
                 * en [rsp+base+j*8]).  El limite 12 era del bytecode/interp;
                 * aqui no existe.  FP overflow (raro en FFI) -> bail. */
                const size_t calln_nargs = in.operands.size();
                const size_t calln_fmax =
                    fp_ok
                        ? tri_sel.arg_regs[static_cast<size_t>(RegClass::FP)]
                              .size()
                        : 0;
                size_t cn_gi = 0, cn_fi = 0;
                bool calln_fp_ovf = false;
                std::vector<MInstr> calln_args;
                for (size_t a = 0; a < calln_nargs; ++a) {
                    const ir::IrValueId av = in.operands[a];
                    const bool is_f = fp_ok && av < fn.values.size() &&
                                      ir_type_is_float(fn.values[av].type);
                    if (is_f) {
                        if (cn_fi >= calln_fmax) {
                            calln_fp_ovf = true;
                            break;
                        }
                        calln_args.push_back(MInstr::make_arg(
                            static_cast<uint8_t>(cn_fi), vrt(av)));
                        ++cn_fi;
                    } else {
                        /* GP: overflow permitido -> stack arg. */
                        calln_args.push_back(MInstr::make_arg(
                            static_cast<uint8_t>(cn_gi), vr(av)));
                        ++cn_gi;
                    }
                }
                if (calln_fp_ovf) {
                    vreg_dbg(fn.name.c_str(), "calln-fp-stack");
                    return false;
                }
                for (auto &mi : calln_args) O.push_back(mi);
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
                    /* kind 0 (SRET_DISPATCH): deleter DINAMICO en
                     * operands[1] (cargado del slot+8 en runtime).
                     *   if (ptr == 0) skip;            // moved
                     *   regs[1]=ptr; regs[15]=1;
                     *   if (deleter == 0) raw_free(ptr);
                     *   else vrt_call_bc_function(proc, deleter);  // VM call
                     * Retira de slots los unique<T> con deleter dinamico. */
                    if (!vm || in.operands.size() < 2 ||
                        ent.call_bc_function == 0 || ent.raw_free == 0) {
                        vreg_dbg(fn.name.c_str(), "smartptr_free(kind0-no-entry)");
                        return false;
                    }
                    const ir::IrValueId deleter = in.operands[1];
                    const MLabelId Lsp_done = out.new_label();
                    const MLabelId Lsp_raw = out.new_label();
#if defined(_WIN32)
                    const MReg sp_pr = MReg::RCX, sp_a1 = MReg::RDX;
#else
                    const MReg sp_pr = MReg::RDI, sp_a1 = MReg::RSI;
#endif
                    O.push_back(mk_test(ptr, ptr));
                    O.push_back(MInstr::make_jcc(MCond::E, Lsp_done));
                    /* stage args del deleter: regs[1]=ptr, regs[15]=1. */
                    O.push_back(
                        MInstr::make_unary(MOp::MOV, vm_reg_mem(1), vr(ptr)));
                    O.push_back(MInstr::make_unary(MOp::MOV, vm_reg_mem(15),
                                                   MOperand::make_imm32(1)));
                    O.push_back(mk_test(deleter, deleter));
                    O.push_back(MInstr::make_jcc(MCond::E, Lsp_raw));
                    /* vrt_call_bc_function(proc, deleter).  R10 temporal. */
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(MReg::R10, 8), vr(deleter)));
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(sp_a1, 8),
                        MOperand::make_reg(MReg::R10, 8)));
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(sp_pr, 8),
                        MOperand::make_reg(MReg::RBX, 8)));
                    O.push_back(MInstr::make_call_abs(
                        out.intern_imm64(ent.call_bc_function)));
                    O.push_back(MInstr::make_jmp(Lsp_done));
                    /* default: raw_free(proc, ptr). */
                    O.push_back(MInstr::make_label_def(Lsp_raw));
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(MReg::R10, 8), vr(ptr)));
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(sp_a1, 8),
                        MOperand::make_reg(MReg::R10, 8)));
                    O.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(sp_pr, 8),
                        MOperand::make_reg(MReg::RBX, 8)));
                    O.push_back(
                        MInstr::make_call_abs(out.intern_imm64(ent.raw_free)));
                    O.push_back(MInstr::make_label_def(Lsp_done));
                    break;
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
                if (abi == AbiKind::HOST_LEAF && in.is_tls) {
                    /* thread_local: la direccion por-hilo se computa via el
                     * thread pointer.  ELF (local-exec): `mov dst,%fs:0` + `lea
                     * dst,[dst+sym@tpoff]` (reloc TPOFF32).  PE (TLS directory):
                     * carga el bloque TLS del TEB con `__vx_tls_index` y suma
                     * el offset del var (SECREL32).  El reloc apunta a
                     * `tdata.<imm>`; el driver/emisor lo resuelven. */
                    const uint32_t vsidx = out.intern_reloc_symbol(
                        "tdata." + std::to_string(in.imm));
                    if (target_sysv) {
                        O.push_back(MInstr::make_tls_le_addr(vr(in.dst), vsidx));
                    } else {
                        const uint32_t isidx =
                            out.intern_reloc_symbol("__vx_tls_index");
                        O.push_back(MInstr::make_tls_pe_addr(vr(in.dst), vsidx,
                                                             isidx));
                    }
                    break;
                }
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
                if (resolve_symbol) {
                    /* Un slot vive en `code` (literales de STRMAKE, params de
                     * los opcodes meta: memoria VM, que es la que esos
                     * consumidores exigen) o en `gdata` (storage de variable
                     * global: memoria host).  El IR embebido no lleva el pool
                     * de static_data, asi que aqui no se sabe cual es: se
                     * prueban los dos nombres -- cada slot existe en una sola
                     * seccion, asi que no hay ambiguedad. */
                    const std::string sfx = ".s_" + std::to_string(in.imm);
                    addr = resolve_symbol("code" + sfx);
                    if (addr == 0) addr = resolve_symbol("gdata" + sfx);
                }
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
                /* FN.3 pieza 1: si la funcion destino YA tiene codigo nativo
                 * JIT (registrado en el pc-map por el force-eager del grafo de
                 * fibra o por el cascade resolver), emitimos el puntero de
                 * codigo NATIVO en vez de la VA de bytecode.  `fiber_entry(fn)`
                 * lo hereda -> el ctx.r12 de una fibra apunta a codigo nativo
                 * VM_ABI (el trampolin salta a el); un cfn `&fn` ya compilado
                 * se llama directo por CALLIND.  Si aun no esta compilada, se
                 * emite la VA como hasta ahora (CALLIND la resuelve a nativo
                 * on-demand; para fibras el force-eager garantiza el native). */
                if (void *jc = lookup_jit_code_at_pc(addr))
                    addr = reinterpret_cast<uint64_t>(jc);
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

            /* FN.3 pieza 3: SWAPCTX en VM_ABI (JIT) -> CALL nativo a
             * `__vx_swapctx` (@Naked, host-ABI).  operands[0]=to_ctx (a
             * cargar), operands[1]=from_ctx (a guardar).  El primitivo toma
             * sus args en los arg-regs NATIVOS (arg0=to, arg1=from), NO en
             * proc->registers -> usamos ARG + CALL_ABS (misma via que un CALLN
             * host-ABI).  ent.swapctx = direccion nativa de __vx_swapctx que
             * el force-eager del grafo de fibra dejo lista (compilada por
             * compile_native_fn).  Es un context-switch: al reanudar (cuando
             * alguien vuelva a esta fibra) los callee-saved los restaura el
             * propio __vx_swapctx; los caller-saved vivos a traves de la
             * llamada los spillea el regalloc (CALL_ABS = call-position).  El
             * path INTERPRETE sigue usando el opcode VM `swapctx` (FN.1); aqui
             * solo el path JIT nativo. */
            case ir::IrOp::SWAPCTX: {
                flush_pending();
                if (abi != AbiKind::VM || ent.swapctx == 0 ||
                    in.operands.size() != 2) {
                    vreg_dbg(fn.name.c_str(), "swapctx(no-native)");
                    return false;
                }
                O.push_back(MInstr::make_arg(0, vr(in.operands[0]))); // to_ctx
                O.push_back(MInstr::make_arg(1, vr(in.operands[1]))); // from_ctx
                O.push_back(
                    MInstr::make_call_abs(out.intern_imm64(ent.swapctx)));
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
