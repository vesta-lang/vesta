/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ir_effects.cpp
 * @brief Motor IR -> SemanticEffects.  El asm lifteado NO es especial: entra
 *        como ADD/LOAD/STORE/... normales.  Solo el residuo opaco (INLINE_ASM/
 *        ASM_MICRO) se analiza aparte, conservador y con tags.
 */
#include "vx/effects/ir_effects.h"

#include "ir/ssa_ir.h"
#include "vx/asm/asm_analyze.h"

#include <algorithm>

namespace vx {
namespace fx {

using ir::IrOp;

// --------------------------------------------------------------------------
// Mapa def-use + clasificacion de punteros a AbstractLoc
// --------------------------------------------------------------------------
IrDefMap build_def_map(const ir::IrFunction &fn) {
    IrDefMap m;
    m.def_of.assign(fn.values.size(), nullptr);
    m.param_of.assign(fn.values.size(), -1);
    for (size_t i = 0; i < fn.params.size(); ++i) {
        const ir::IrValueId p = fn.params[i];
        if (p < m.param_of.size()) m.param_of[p] = static_cast<int32_t>(i);
    }
    for (const ir::IrBlock &b : fn.blocks)
        for (const ir::IrInstr &in : b.instrs)
            if (in.dst != ir::IR_NO_VALUE && in.dst < m.def_of.size())
                m.def_of[in.dst] = &in;
    m.built = true;
    return m;
}

static AbstractLoc classify_rec(const ir::IrFunction &fn, const IrDefMap &defs,
                                ir::IrValueId ptr, int depth) {
    using K = AbstractLoc::Kind;
    if (ptr == ir::IR_NO_VALUE || ptr >= defs.def_of.size())
        return {K::Unknown, 0};
    if (defs.param_of[ptr] >= 0)
        return {K::ArgDerived, static_cast<uint32_t>(defs.param_of[ptr])};
    const ir::IrInstr *d = defs.def_of[ptr];
    if (!d || depth <= 0) return {K::Unknown, 0};
    switch (d->op) {
    case IrOp::ALLOCA:
        return {K::Stack, ptr}; // cada ALLOCA es un slot distinto
    case IrOp::RAW_ALLOC:
    case IrOp::GC_ALLOC:
    case IrOp::GC_ALLOCP:
    case IrOp::NEWOBJ:
    case IrOp::ARRAY_ALLOC:
    case IrOp::STRMAKE:
    case IrOp::STRRESERVE:
        return {K::Heap, ptr}; // cada alloc-site es distinto (id = value id)
    case IrOp::GETSTATIC:
    case IrOp::STR_LIT_ADDR:
    case IrOp::LABEL_ADDR:
    case IrOp::SECTION_REF:
        return {K::Global, ptr};
    // Derivaciones: el loc es el de la BASE (operands[0]).
    case IrOp::GEP:
    case IrOp::BITCAST:
    case IrOp::CAST:
    case IrOp::MVTAKE_IR:
    case IrOp::GCDEREF_IR:
    case IrOp::GC_DEREF_HOST:
    case IrOp::GC_HANDLE_FOR_PTR:
    case IrOp::UNWRAP:
        if (!d->operands.empty())
            return classify_rec(fn, defs, d->operands[0], depth - 1);
        return {K::Unknown, 0};
    default:
        return {K::Unknown, 0}; // cargado de memoria, PHI, const-address, ...
    }
}

AbstractLoc classify_ptr(const ir::IrFunction &fn, const IrDefMap &defs,
                         ir::IrValueId ptr) {
    return classify_rec(fn, defs, ptr, 8);
}

// --------------------------------------------------------------------------
// Efecto local de una instruccion opaca de asm (INLINE_ASM / ASM_MICRO).
// --------------------------------------------------------------------------
static EffectAnalysisResult opaque_asm_effects(const ir::IrInstr &ins) {
    EffectAnalysisResult r;
    // func_name lleva el cuerpo NASM (lo pone el lowering de asm).
    const AsmBlockEffects e = asm_analyze_block(ins.func_name, "x86_64");
    if (e.touches_mem) {
        r.effects.mem.reads.add({AbstractLoc::Kind::Unknown, LOC_GENERIC});
        r.effects.mem.writes.add({AbstractLoc::Kind::Unknown, LOC_GENERIC});
    }
    if (e.is_call) {
        r.effects.control.kind = ControlKind::Call;
        r.effects.may_io = true; // un call opaco puede hacer cualquier cosa
    }
    if (e.has_atomic) {
        r.effects.atomic.order = MemOrder::SeqCst;
        r.effects.atomic.is_fence = true;
        r.effects.tags.add(CapabilityTag::UserBarrier);
    }
    // Un asm que no toca mem, no llama y no es atomico es puro (aritmetica sobre
    // registros): efecto neutro.  Un mnemonico DESCONOCIDO no se puede acotar ->
    // efecto MAXIMO robusto (podria hacer cualquier cosa) + LAGUNA a reportar.
    if (!e.known()) {
        r.effects = SemanticEffects::top();
        r.completeness = AnalysisCompleteness::Unknown;
        r.unknown_reason = UnknownReason::UnknownMnemonic;
    } else {
        r.completeness = AnalysisCompleteness::Conservative;
    }
    return r;
}

// --------------------------------------------------------------------------
// Efecto local de UNA instruccion IR.
// --------------------------------------------------------------------------
static void add_read(SemanticEffects &e, const AbstractLoc &l) {
    e.mem.reads.add(l);
}
static void add_write(SemanticEffects &e, const AbstractLoc &l) {
    e.mem.writes.add(l);
}

EffectAnalysisResult effects_of_instr(const ir::IrFunction &fn,
                                      const IrDefMap &defs,
                                      const ir::IrInstr &ins) {
    EffectAnalysisResult r; // neutro Complete por defecto
    SemanticEffects &e = r.effects;
    const auto &ops = ins.operands;

    switch (ins.op) {
    // ---- Computacion pura (sin efectos observables) ----
    case IrOp::CONST: case IrOp::MOV: case IrOp::NOP:
    case IrOp::ADD: case IrOp::SUB: case IrOp::MUL:
    case IrOp::NEG: case IrOp::IABS: case IrOp::IMIN: case IrOp::IMAX:
    case IrOp::IMINU: case IrOp::IMAXU:
    case IrOp::FADD: case IrOp::FSUB: case IrOp::FMUL: case IrOp::FDIV:
    case IrOp::FNEG: case IrOp::FABS: case IrOp::FSQRT: case IrOp::FMIN:
    case IrOp::FMAX: case IrOp::FFLOOR: case IrOp::FCEIL: case IrOp::FROUND:
    case IrOp::FTRUNC: case IrOp::VEC_UNOP: case IrOp::VEC_BINOP: case IrOp::VEC_FMA:
    case IrOp::AND: case IrOp::OR: case IrOp::XOR: case IrOp::NOT:
    case IrOp::SHL: case IrOp::SHR: case IrOp::SAR: case IrOp::CLZ: case IrOp::CTZ:
    case IrOp::POPCNT: case IrOp::BYTESWAP: case IrOp::ROTL: case IrOp::ROTR:
    case IrOp::CMP_EQ: case IrOp::CMP_NE: case IrOp::CMP_LT: case IrOp::CMP_GT:
    case IrOp::CMP_LE: case IrOp::CMP_GE: case IrOp::CMP_ULT: case IrOp::CMP_UGT:
    case IrOp::CMP_ULE: case IrOp::CMP_UGE:
    case IrOp::FCMP_EQ: case IrOp::FCMP_NE: case IrOp::FCMP_LT: case IrOp::FCMP_GT:
    case IrOp::FCMP_LE: case IrOp::FCMP_GE:
    case IrOp::CAST: case IrOp::ZEXT: case IrOp::SEXT: case IrOp::TRUNC:
    case IrOp::ITOF: case IrOp::UITOF: case IrOp::FTOI: case IrOp::FTOUI:
    case IrOp::BITCAST: case IrOp::PHI: case IrOp::ALLOCA: case IrOp::GEP:
    case IrOp::STR_LIT_ADDR: case IrOp::LABEL_ADDR: case IrOp::SECTION_REF:
    case IrOp::ISNULL: case IrOp::INSTANCEOF:
    // Conversiones de puntero/handle: solo calculan una direccion (el load/store
    // real es una op aparte); sin efecto observable propio.
    case IrOp::GCDEREF_IR: case IrOp::GC_DEREF_HOST: case IrOp::GC_HANDLE_FOR_PTR:
    // Metadata de depuracion: no afecta la semantica de datos del programa.
    case IrOp::SETMETHDBG:
        break; // efecto neutro

    // ---- Division: puede atrapar (div-by-zero) ----
    case IrOp::DIV: case IrOp::MOD:
        e.may_trap = true;
        break;

    // ---- Memoria ----
    case IrOp::LOAD:
        if (!ops.empty()) add_read(e, classify_ptr(fn, defs, ops[0]));
        break;
    case IrOp::STORE:
        if (ops.size() >= 2) add_write(e, classify_ptr(fn, defs, ops[1]));
        break;
    case IrOp::ARRAY_LOAD:
        if (!ops.empty()) add_read(e, classify_ptr(fn, defs, ops[0]));
        break;
    case IrOp::ARRAY_STORE:
        if (!ops.empty()) add_write(e, classify_ptr(fn, defs, ops[0]));
        break;
    case IrOp::GETFIELD:
        if (!ops.empty()) add_read(e, classify_ptr(fn, defs, ops[0]));
        break;
    case IrOp::SETFIELD:
    case IrOp::GCWB_IR:
        if (!ops.empty()) add_write(e, classify_ptr(fn, defs, ops[0]));
        break;
    case IrOp::GETSTATIC:
        add_read(e, {AbstractLoc::Kind::Global, LOC_GENERIC});
        break;
    case IrOp::SETSTATIC:
        add_write(e, {AbstractLoc::Kind::Global, LOC_GENERIC});
        break;
    case IrOp::ARRAY_LEN: case IrOp::STRLEN: case IrOp::STRGETBYTES:
    case IrOp::STRHASH:
        // leen la cabecera del objeto (heap).
        if (!ops.empty()) add_read(e, classify_ptr(fn, defs, ops[0]));
        break;
    case IrOp::MEMCPY:
        add_read(e, {AbstractLoc::Kind::Unknown, LOC_GENERIC});
        add_write(e, {AbstractLoc::Kind::Unknown, LOC_GENERIC});
        break;

    // ---- Asignacion de memoria (aloca heap) ----
    case IrOp::RAW_ALLOC: case IrOp::GC_ALLOC: case IrOp::GC_ALLOCP:
    case IrOp::NEWOBJ: case IrOp::ARRAY_ALLOC: case IrOp::MAKE_CLOSURE:
    case IrOp::STRMAKE: case IrOp::STRCAT: case IrOp::STRCONV: case IrOp::STRSLICE:
    case IrOp::STRFLAT: case IrOp::STRINTERN: case IrOp::STRRESERVE:
    case IrOp::MAKE_VARIANT: case IrOp::SPECIALIZE: case IrOp::FUTURE:
        e.may_allocate = true;
        break;

    // ---- Liberacion: invalida memoria (conservador: escribe Unknown) ----
    case IrOp::RAW_FREE: case IrOp::SMARTPTR_FREE:
        add_write(e, {AbstractLoc::Kind::Unknown, LOC_GENERIC});
        break;
    case IrOp::GC_COLLECT: case IrOp::GC_FINALIZE_ALL:
        add_write(e, {AbstractLoc::Kind::Unknown, LOC_GENERIC});
        break;

    // ---- Excepciones ----
    case IrOp::THROW: case IrOp::RETHROW: case IrOp::PANIC:
        e.may_throw = true;
        e.control.kind = ControlKind::Throw;
        break;
    case IrOp::UNWRAP:    // NullPointerException si null
    case IrOp::CHECKCAST: // ClassCastException
        e.may_throw = true;
        break;

    // ---- Control ----
    case IrOp::RET:
        e.control.kind = ControlKind::Return;
        break;
    case IrOp::BR: case IrOp::BR_COND: case IrOp::SWITCH_DENSE:
    case IrOp::MATCH_VARIANT:
        e.control.kind = ControlKind::Branch;
        break;
    case IrOp::UNREACHABLE:
        e.control.kind = ControlKind::NoReturn;
        break;

    // ---- Llamadas.  El efecto LOCAL es 'transfiere control'; el efecto del
    // callee entra por el cierre (Fase 2).  Las dinamicas/nativas son opacas. ----
    case IrOp::CALL: case IrOp::TAILCALL:
        e.control.kind = ControlKind::Call;
        break;
    case IrOp::CALLVIRT: case IrOp::CALLM: case IrOp::CALLITF:
    case IrOp::CALLCLOSURE:
        e.control.kind = ControlKind::Call;
        r.completeness = AnalysisCompleteness::Conservative; // callee dinamico
        r.unknown_reason = UnknownReason::DynamicDispatch;
        break;
    case IrOp::CALLIND:
        e.control.kind = ControlKind::Call;
        r.completeness = AnalysisCompleteness::Conservative;
        r.unknown_reason = UnknownReason::Indirect;
        break;
    case IrOp::CALLN: // FFI/nativo: caja negra -> efecto MAXIMO robusto.
        e = SemanticEffects::top();
        r.completeness = AnalysisCompleteness::Conservative;
        r.unknown_reason = UnknownReason::UnknownFFI;
        break;

    // ---- Concurrencia ----
    case IrOp::AWAIT: case IrOp::MONENTER: case IrOp::MONWAIT: case IrOp::MSGRECV:
        e.may_block = true;
        break;
    case IrOp::MONEXIT:
        e.atomic.order = MemOrder::Release;
        break;
    case IrOp::MSGSEND: case IrOp::FULFILL: case IrOp::REJECT:
    case IrOp::FULFILL_HLT:
        e.may_io = true; // comunicacion observable
        break;
    case IrOp::SPAWN: case IrOp::SPAWN_ARGS: case IrOp::RSPAWN:
        e.may_allocate = true; // crea proceso
        e.may_io = true;
        break;
    case IrOp::YIELD: case IrOp::RESUME:
        e.control.kind = ControlKind::Suspend;
        break;

    // ---- Estado del proceso / entorno (no determinista) ----
    case IrOp::GETPROC: case IrOp::GETVM: case IrOp::READ_VM_REG:
        e.determinism.add(DeterminismTag::ExternalObservable);
        break;

    // ---- I/O / carga dinamica ----
    case IrOp::DLOPEN: case IrOp::DLSYM: case IrOp::MOD_LOAD:
        e.may_io = true;
        r.completeness = AnalysisCompleteness::Conservative;
        break;

    // ---- Reflexion / registro de clases (muta el ClassRegistry) ----
    case IrOp::DEFCLASS: case IrOp::DEFFIELD: case IrOp::DEFMETHOD:
    case IrOp::ADDADVICE:
        add_write(e, {AbstractLoc::Kind::Global, LOC_GENERIC});
        e.may_allocate = true;
        break;
    case IrOp::FINDCLASS: case IrOp::FINDMETHOD:
        add_read(e, {AbstractLoc::Kind::Global, LOC_GENERIC});
        break;

    // ---- Residuo de asm OPACO ----
    case IrOp::INLINE_ASM: case IrOp::ASM_MICRO:
        return opaque_asm_effects(ins);

    default:
        // Op no clasificada -> efecto MAXIMO (top): robusto y completo, cubre
        // CUALQUIER cosa que la op pueda hacer.  Nunca afirma de menos.  Marca
        // UnmodeledOp: es una LAGUNA del motor (deberiamos clasificar esta op
        // para ganar precision), NO una opacidad fundamental.
        e = SemanticEffects::top();
        r.completeness = AnalysisCompleteness::Conservative;
        r.unknown_reason = UnknownReason::UnmodeledOp;
        break;
    }
    return r;
}

// --------------------------------------------------------------------------
// Agregado local de una funcion: seq dentro de bloque, join entre bloques.
// --------------------------------------------------------------------------
EffectAnalysisResult function_local_effects(const ir::IrFunction &fn,
                                            EffectGaps *gaps) {
    IrDefMap defs = build_def_map(fn);
    EffectAnalysisResult acc;
    bool first_block = true;
    AnalysisCompleteness worst = AnalysisCompleteness::Complete;

    for (const ir::IrBlock &b : fn.blocks) {
        SemanticEffects blk = SemanticEffects::none();
        bool first_instr = true;
        for (const ir::IrInstr &in : b.instrs) {
            EffectAnalysisResult r = effects_of_instr(fn, defs, in);
            if (uint8_t(r.completeness) > uint8_t(worst)) worst = r.completeness;
            // Registrar la laguna (si la hubo) para el reporte de cobertura.
            if (gaps && r.completeness != AnalysisCompleteness::Complete &&
                r.unknown_reason != UnknownReason::None)
                gaps->record(static_cast<int>(in.op), r.unknown_reason);
            blk = first_instr ? r.effects : seq(blk, r.effects);
            first_instr = false;
        }
        acc.effects = first_block ? blk : join(acc.effects, blk);
        first_block = false;
    }
    acc.completeness = worst;
    return acc;
}

} // namespace fx
} // namespace vx
