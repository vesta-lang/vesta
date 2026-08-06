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
#include "analysis/effects/ir_effects.h"
#include "analysis/memory/memory_access.h"

#include "ir/ssa_ir.h"
#include "vx/asm/asm_analyze.h"
#include "vx/asm/asm_effects.h" // canonicalizar el registro base (por arch)

#include <algorithm>

namespace analysis {
namespace effects {

using ir::IrOp;

// --------------------------------------------------------------------------
// Clasificacion de punteros a AbstractLoc -- delega en el RESOLVEDOR COMPARTIDO
// (analysis/memory/points_to), que es la unica fuente de "a que memoria apunta
// este puntero" para efectos Y para el DSE.  Antes habia aqui un classify_rec
// duplicado; se elimino para tener un solo modelo.
// --------------------------------------------------------------------------
AbstractLoc classify_ptr(const ir::IrFunction &fn, const analysis::IrFacts &facts,
                         ir::IrValueId ptr) {
    // Conveniencia (tests / llamadas sueltas): construye una tabla local.  El
    // camino caliente (por-instr) usa la tabla cacheada via effects_of_instr.
    analysis::PointsTo pt = analysis::compute_points_to(fn, facts);
    return analysis::loc_of(pt, ptr, 0 /*ancho desconocido = objeto entero*/);
}

// Bytes accedidos por un LOAD/STORE: delega en la UNICA verdad compartida.
static int32_t access_bytes(ir::IrType t) {
    return analysis::memory_access_size(t);
}

// --------------------------------------------------------------------------
// Efecto local de una instruccion opaca de asm (INLINE_ASM / ASM_MICRO).
// --------------------------------------------------------------------------
static EffectAnalysisResult opaque_asm_effects(const ir::IrFunction &fn,
                                              const analysis::PointsTo &pt,
                                              const ir::IrInstr &ins) {
    EffectAnalysisResult r;
    // func_name lleva el cuerpo NASM (lo pone el lowering de asm).  El analisis
    // de bloque del asm opaco vive en el modulo asm (namespace vx).
    const vx::AsmBlockEffects e = vx::asm_analyze_block(ins.func_name, "x86_64");
    /* Se declara SOLO lo que el bloque hace.  Antes, cualquier asm que tocara
     * memoria se anotaba como lectura Y escritura de todo, y eso lo convierte
     * en una barrera para cuanto haya alrededor: un `mov rax, [rdi]` impedia
     * mover una escritura, subir una lectura fuera de un bucle o eliminar una
     * escritura muerta.  El analisis del asm ya distingue las dos cosas -- la
     * tabla dice que operandos escribe cada instruccion -- y ante cualquier
     * duda marca las dos, asi que esto no afloja nada. */
    /* Y se dice QUE memoria cuando se puede.  El bloque llega a ella por un
     * registro, y ese registro esta LIGADO a una variable del programa, asi que
     * hay camino: registro -> ligadura -> hueco de la variable -> lo que se
     * guardo en el.  Con eso, un `asm` que escribe en `[rdi]` afirma la
     * localizacion de `*q` en vez de "cualquier sitio", y deja de estorbar a lo
     * que toca OTRA memoria.
     *
     * Solo si se pueden atribuir TODOS los accesos: uno sin atribuir significa
     * que el bloque toca algo que no sabemos nombrar, y entonces la lista no
     * describe el total.  Igual con las ligaduras que aun no tienen registro
     * (lo elige el asignador despues): no se pueden emparejar por nombre. */
    bool localizado = !e.accesos.empty() && !e.accesos_incompletos;
    std::vector<AbstractLoc> locs_lee, locs_escribe;
    if (localizado) {
        for (const vx::AsmBlockEffects::Acceso &a : e.accesos) {
            /* La lista de ligaduras es de TODA la funcion, no del ambito de
             * este bloque: si dos variables de ambitos distintos usan el mismo
             * registro, quedarse con la primera seria elegir a ciegas.  Con mas
             * de una candidata no se afirma nada. */
            ir::IrValueId hueco = ir::IR_NO_VALUE;
            unsigned candidatas = 0;
            for (const ir::AsmRegBinding &b : fn.asm_reg_bindings)
                if (!b.reg.empty() && vx::asm_canonical_reg(b.reg) == a.base) {
                    hueco = b.alloca_value;
                    ++candidatas;
                }
            if (candidatas != 1) { localizado = false; break; }
            const ir::IrValueId valor =
                analysis::valor_unico_del_hueco(fn, hueco);
            if (valor == ir::IR_NO_VALUE) { localizado = false; break; }
            // Ancho 0: el bloque puede tocar todo el objeto, no un campo.
            const AbstractLoc l = analysis::loc_of(pt, valor, 0);
            if (l.kind == AbstractLoc::Kind::Unknown) { localizado = false; break; }
            if (a.escribe) locs_escribe.push_back(l);
            else locs_lee.push_back(l);
        }
    }
    if (localizado) {
        for (const AbstractLoc &l : locs_lee) r.effects.mem.reads.add(l);
        for (const AbstractLoc &l : locs_escribe) {
            r.effects.mem.writes.add(l);
            // Escribir por un puntero es tambien leer por el (ver el analisis).
            r.effects.mem.reads.add(l);
        }
    } else {
        if (e.reads_mem)
            r.effects.mem.reads.add({AbstractLoc::Kind::Unknown, LOC_GENERIC});
        if (e.writes_mem)
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
                                      const analysis::IrFacts &facts,
                                      const analysis::PointsTo &pt,
                                      const ir::IrInstr &ins) {
    (void)facts; // el points-to (pt) ya se construyo con los hechos.
    EffectAnalysisResult r; // neutro Complete por defecto
    SemanticEffects &e = r.effects;
    const auto &ops = ins.operands;
    // Localizacion de un puntero-operando con el ancho del acceso actual.
    const int32_t w = access_bytes(ins.type);
    auto loc = [&](ir::IrValueId p, int32_t width) {
        return analysis::loc_of(pt, p, width);
    };

    switch (ins.op) {
    // ---- Computacion pura (sin efectos observables) ----
    case IrOp::CONST: case IrOp::MOV: case IrOp::NOP:
    case IrOp::ADD: case IrOp::SUB: case IrOp::MUL:
    case IrOp::NEG: case IrOp::IABS: case IrOp::IMIN: case IrOp::IMAX:
    case IrOp::IMINU: case IrOp::IMAXU:
    case IrOp::FADD: case IrOp::FSUB: case IrOp::FMUL: case IrOp::FDIV:
    case IrOp::FNEG: case IrOp::FABS: case IrOp::FSQRT: case IrOp::FMIN:
    case IrOp::FMAX: case IrOp::FFLOOR: case IrOp::FCEIL: case IrOp::FROUND:
    case IrOp::FTRUNC:
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

    // ---- Memoria (localizacion precisa: raiz + offset + ancho del acceso) ----
    case IrOp::LOAD:
        if (!ops.empty()) add_read(e, loc(ops[0], w));
        break;
    case IrOp::STORE:
        if (ops.size() >= 2) add_write(e, loc(ops[1], w));
        break;
    case IrOp::ARRAY_LOAD:
        if (!ops.empty()) add_read(e, loc(ops[0], w));
        break;
    case IrOp::ARRAY_STORE:
        if (!ops.empty()) add_write(e, loc(ops[0], w));
        break;
    case IrOp::GETFIELD:
        if (!ops.empty()) add_read(e, loc(ops[0], w));
        break;
    case IrOp::SETFIELD:
    case IrOp::GCWB_IR:
        if (!ops.empty()) add_write(e, loc(ops[0], w));
        break;
    case IrOp::GETSTATIC:
        add_read(e, {AbstractLoc::Kind::Global, LOC_GENERIC});
        break;
    case IrOp::SETSTATIC:
        add_write(e, {AbstractLoc::Kind::Global, LOC_GENERIC});
        break;
    case IrOp::ARRAY_LEN: case IrOp::STRLEN: case IrOp::STRGETBYTES:
    case IrOp::STRHASH:
    // STRRAW: devuelve un host_ptr al buffer de datos del StringObject -> LEE el
    // objeto (cabecera+datos) para calcular el puntero; no escribe/aloca/lanza.
    // Una escritura POSTERIOR via el puntero devuelto es un STORE aparte
    // (modelado).  Sin esto, strraw subia a top() (laguna modelable).
    case IrOp::STRRAW:
        // leen la cabecera del objeto (ancho desconocido = objeto entero).
        if (!ops.empty()) add_read(e, loc(ops[0], 0));
        break;
    // MEMCPY + ops VECTORIALES: delegan en el vocabulario UNICO memory_access
    // (memcpy NO es opaco; los VEC ESCRIBEN memoria -- antes VEC_UNOP/BINOP/FMA
    // estaban mal clasificados como PUROS, lo que podia clasificar una funcion
    // que solo hace stores vectoriales como "pura" -> unsound en las
    // relajaciones pure-call).  opaco -> top; VEC_BCAST no toca memoria.
    case IrOp::MEMCPY:
    case IrOp::MEMSET:
    case IrOp::VEC_UNOP: case IrOp::VEC_BINOP: case IrOp::VEC_BINOP_S:
    case IrOp::VEC_FMA: case IrOp::VEC_BCAST:
    case IrOp::VEC_ACC_ZERO: case IrOp::VEC_ACC_ADD: case IrOp::VEC_ACC_FMA:
    case IrOp::VEC_ACC_STORE: case IrOp::VEC_ACC_COMBINE: {
        const analysis::MemoryAccess ma = analysis::memory_access(ins, pt);
        if (ma.touches) {
            if (ma.opaque) {
                if (ma.is_load) add_read(e, {AbstractLoc::Kind::Unknown, LOC_GENERIC});
                if (ma.is_store) add_write(e, {AbstractLoc::Kind::Unknown, LOC_GENERIC});
            } else {
                for (const auto &r : ma.reads) add_read(e, r);
                for (const auto &w : ma.writes) add_write(e, w);
            }
        }
        break;
    }

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
        return opaque_asm_effects(fn, pt, ins);

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
    analysis::IrFacts facts = analysis::build_ir_facts(fn);
    analysis::PointsTo pt = analysis::compute_points_to(fn, facts);
    EffectAnalysisResult acc;
    bool first_block = true;
    AnalysisCompleteness worst = AnalysisCompleteness::Complete;

    for (const ir::IrBlock &b : fn.blocks) {
        SemanticEffects blk = SemanticEffects::none();
        bool first_instr = true;
        for (const ir::IrInstr &in : b.instrs) {
            EffectAnalysisResult r = effects_of_instr(fn, facts, pt, in);
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

} // namespace effects
} // namespace analysis
