/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file evaluable.cpp
 * @brief Implementacion del analisis de evaluabilidad CTPE (ver evaluable.h).
 *
 * Recordatorio del PRINCIPIO RECTOR: se clasifica por AISLAMIENTO del universo
 * temporal del ComptimeRuntime, NO por pureza.  Mutar globals/heap/GC/objetos
 * es evaluable (ese mundo se destruye al terminar); solo descalifica que un
 * efecto ESCAPE (I/O/entorno/FFI/distribucion) o cambie la ESTRUCTURA del
 * programa.
 */

#include "ctpe/evaluable.h"

#include "ir/ir_type_info.h" // vocabulario UNICO de anchura/clase de un IrType

namespace ctpe {

using ir::IrOp;

// Politica por AISLAMIENTO (no por pureza).  Por defecto @c Always: la inmensa
// mayoria de ops (aritmetica, memoria/alloc local, GC, POO, strings, reflexion
// de consulta, excepciones, atomics sobre memoria privada, async LOCAL) quedan
// contenidas en la VM de compilacion.  Solo se enumeran las que NO lo estan.
CtpePolicy ctpe_policy(IrOp op) {
    switch (op) {
    // 🟡 Snapshot: muta el estado global del comptime; se descarta al terminar
    // (o, fase 2, se serializa a .rodata).  Contenido -> evaluable.
    case IrOp::SETSTATIC: return CtpePolicy::Snapshot;

    // 🔴 ExternalIO: I/O externo / FFI nativa / carga dinamica / asm opaco.
    case IrOp::CALLN: // llamada a funcion nativa (io, syscalls, ...).
    case IrOp::DLOPEN:
    case IrOp::DLSYM:
    case IrOp::MOD_LOAD: // carga un .velb externo del filesystem.
    case IrOp::ASM_MICRO:
    case IrOp::INLINE_ASM:
    case IrOp::RAW_ASM: // opaco: no se puede garantizar nada.
        return CtpePolicy::ExternalIO;

    // 🔴 NeedsHost: depende del ENTORNO/host o de la distribucion remota.
    case IrOp::GETPID:
    case IrOp::GETARGC:
    case IrOp::GETARG: // argv: entrada NO constante (depende de la ejecucion).
    case IrOp::GETPROC:
    case IrOp::GETVM:
    case IrOp::GETMGR:
    case IrOp::READ_VM_REG:
    case IrOp::RSPAWN: // spawn REMOTO (otro nodo).
    case IrOp::RSPAWN_RETURN:
    case IrOp::SHARED_STAT: // memoria compartida cross-proceso.
    case IrOp::GC_PROMOTE:
    case IrOp::GC_DEMOTE:
    case IrOp::NEWOBJS: // alloc en el SharedHeap (cross-proceso).
        return CtpePolicy::NeedsHost;

        // NOTA (paradigma de aislamiento):
        // DEFCLASS/DEFFIELD/DEFMETHOD/ADDADVICE/ SPECIALIZE/SETMETHDBG NO se
        // bloquean.  Definen clases/metodos en el registry de la VM de
        // compilacion, que se DESTRUYE al terminar -> CONTENIDO, no escapa.  Es
        // el arranque NORMAL de un programa OOP (__module_init), no
        // metaprogramacion que persista: CTPE solo inyecta el RESULTADO escalar
        // y descarta el registry entero.  Por eso caen en `default -> Always`.
        // La categoria MetaMutation queda RESERVADA para el dia que CTPE pueda
        // emitir ESTRUCTURA al binario (fase de serializacion), donde si seria
        // metaprog.

    default: return CtpePolicy::Always;
    }
}

Evaluability compute_evaluability(const ir::IrModule &mod) {
    Evaluability out;

    // Indice nombre -> funcion (para resolver CALL directas).
    std::unordered_map<std::string, const ir::IrFunction *> by_name;
    by_name.reserve(mod.functions.size());
    for (const auto &fn : mod.functions)
        by_name[fn.name] = &fn;

    // Paso 1: razon LOCAL de cada funcion (op no-contenida directa).  Si no la
    // hay, queda como candidata inicial (pendiente del fixpoint transitivo).
    std::unordered_set<std::string> candidate;
    for (const auto &fn : mod.functions) {
        if (fn.is_native) {
            BlockReason r;
            r.op = IrOp::CALLN; // proxy: una fn nativa es I/O/host externo.
            r.policy = CtpePolicy::ExternalIO;
            out.reason[fn.name] = r;
            continue;
        }
        bool blocked = false;
        for (ir::IrBlockId b = 0; b < fn.blocks.size() && !blocked; ++b) {
            const auto &blk = fn.blocks[b];
            for (size_t i = 0; i < blk.instrs.size(); ++i) {
                const ir::IrInstr &in = blk.instrs[i];
                if (op_contained(in.op)) continue;
                BlockReason r;
                r.op = in.op;
                r.policy = ctpe_policy(in.op);
                r.block = b;
                r.instr_index = i;
                r.source_line = in.source_line;
                out.reason[fn.name] = r;
                blocked = true;
                break;
            }
        }
        if (!blocked) candidate.insert(fn.name);
    }

    // Paso 2: fixpoint transitivo.  Una candidata deja de serlo si llama
    // DIRECTAMENTE (CALL/TAILCALL con func_name resoluble) a una funcion NO
    // evaluable.  El dispatch dinamico (CALLVIRT/CALLIND/CALLCLOSURE) NO se
    // comprueba: su destino real lo verifica el trap del sandbox en ejecucion.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = candidate.begin(); it != candidate.end();) {
            const ir::IrFunction *fn = by_name[*it];
            bool ok = true;
            BlockReason bad;
            for (ir::IrBlockId b = 0; b < fn->blocks.size() && ok; ++b) {
                const auto &blk = fn->blocks[b];
                for (size_t i = 0; i < blk.instrs.size(); ++i) {
                    const ir::IrInstr &in = blk.instrs[i];
                    if (in.op != IrOp::CALL && in.op != IrOp::TAILCALL)
                        continue;
                    if (in.func_name.empty()) continue; // no resoluble.
                    auto cit = by_name.find(in.func_name);
                    if (cit == by_name.end())
                        continue; // externa desconocida: la
                                  // cubre el trap.
                    if (candidate.count(in.func_name)) continue; // callee OK.
                    // callee NO evaluable -> esta funcion tampoco.
                    ok = false;
                    bad.op = in.op;
                    bad.policy =
                        CtpePolicy::Always; // bloquea por transitividad.
                    bad.block = b;
                    bad.instr_index = i;
                    bad.source_line = in.source_line;
                    bad.callee = in.func_name;
                    break;
                }
            }
            if (!ok) {
                out.reason[*it] = bad;
                it = candidate.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }

    out.evaluable = std::move(candidate);
    return out;
}

// Tipo escalar inyectable como CONST (int/float/bool).  VOID/PTR/HANDLE no lo
// son (un puntero comptime no tiene sentido en runtime; agregados -> fase 2).
static bool is_scalar(ir::IrType t) {
    // Un escalar es un entero o un flotante; ambas preguntas las contesta el
    // vocabulario unico, y un puntero o un handle no son ninguna de las dos.
    return ir::type_is_integer(t) || ir::type_is_float(t);
}

std::vector<Candidate> find_candidates(const ir::IrModule &mod,
                                       const Evaluability &ev) {
    // CTPE = ejecutar el PROGRAMA entero desde su punto de entrada (main) en un
    // solo universo comptime y plegar SU resultado.  Las funciones auxiliares
    // corren DENTRO de esa ejecucion (mismo universo, efectos correctos entre
    // llamadas) y NUNCA se pliegan por separado -- plegar una funcion aislada
    // seria CTFE (evaluacion de funcion), no CTPE, y romperia cualquier
    // auxiliar con estado por-llamada (p.ej. `g_counter += 1; return
    // g_counter;` llamada en bucle daria siempre el mismo valor).  El unico
    // candidato es el entry.
    std::vector<Candidate> out;
    for (const auto &fn : mod.functions) {
        if (fn.name != "main") continue; // solo el punto de entrada.
        if (!ev.is_evaluable(fn.name))
            continue;                     // sin I/O/FFI/host/distribucion.
        if (!fn.params.empty()) continue; // main(argv) lee argv -> no const.
        if (!is_scalar(fn.ret_type)) continue; // retorno escalar inyectable.
        out.push_back({fn.name, fn.ret_type});
    }
    return out;
}

} // namespace ctpe
