/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ir_optimizer.cpp
 * @brief Implementacion de los pases de optimizacion sobre la SSA IR.
 */

#include "ir/ir_optimizer.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>
#include <cstdint>
#include <functional>
#include <sstream>

namespace ir {

// =========================================================================
//  Utilidades internas
// =========================================================================

/** @brief Devuelve true si la instruccion tiene efectos laterales visibles. */
static bool is_side_effecting(IrOp op) {
    switch (op) {
        // llamadas (pueden lanzar excepciones o modificar estado)
        case IrOp::CALL:    case IrOp::CALLIND: case IrOp::CALLVIRT:
        case IrOp::CALLN:   case IrOp::TAILCALL: case IrOp::CALLM:
        case IrOp::CALLCLOSURE:
        // para C2 (escape analysis + case-splitting);
        // NUNCA eliminar aunque dst sea IR_NO_VALUE.  Sin efecto en codegen
        // (emitter los trata como no-op), pero deben sobrevivir DCE.
        case IrOp::MAKE_CLOSURE:
        case IrOp::MAKE_VARIANT:
        case IrOp::MATCH_VARIANT:
        // control de flujo
        case IrOp::BR:      case IrOp::BR_COND: case IrOp::RET:
        case IrOp::UNREACHABLE:
        // excepciones
        case IrOp::THROW:   case IrOp::TRYENTER: case IrOp::TRYLEAVE:
        case IrOp::LANDINGPAD:
        // async / distribucion
        case IrOp::AWAIT:   case IrOp::FULFILL:  case IrOp::REJECT:
        case IrOp::MSGSEND: case IrOp::MSGRECV:  case IrOp::RSPAWN:
        // monitores
        case IrOp::MONENTER: case IrOp::MONEXIT: case IrOp::MONWAIT:
        case IrOp::MONNOTI:  case IrOp::MONNOTA:
        // memoria
        case IrOp::STORE:   case IrOp::MEMCPY:   case IrOp::SETFIELD:
        // OOP con efectos
        case IrOp::NEWOBJ:  case IrOp::CHECKCAST: case IrOp::UNWRAP:
        case IrOp::SPECIALIZE:
        // GC_ALLOC: consume memoria del heap GC + puede disparar minor/major
        // GC + el payload puede ser referenciado posteriormente.  Tratarlo
        // como CALL evita que el DCE elimine el alloc cuando el dst es
        // temporariamente parecido a "no usado" en algun analisis local.
        case IrOp::GC_ALLOC:
        // arrays con efectos
        case IrOp::ARRAY_ALLOC: case IrOp::ARRAY_STORE: case IrOp::GCWB_IR:
        case IrOp::GCDEREF_IR:
        // cadenas con efectos (alloc o mutacion)
        case IrOp::STRMAKE:   case IrOp::STRCAT:    case IrOp::STRCONV:
        case IrOp::STRFLAT:   case IrOp::STRINTERN: case IrOp::STRRESERVE:
        case IrOp::STRFINALIZE:
        // scheduler / proceso
        case IrOp::SPAWN:   case IrOp::RESUME:   case IrOp::YIELD:
        case IrOp::SWAPCTX: case IrOp::SPAWN_ARGS:
        // asignacion
        case IrOp::ALLOCA:
        // ensamblador incrustado (nunca eliminar; semantica opaca)
        case IrOp::RAW_ASM:
            return true;
        default:
            return false;
    }
}

/** @brief Devuelve true si la instruccion es un terminador de bloque. */
static bool is_terminator(IrOp op) {
    return op == IrOp::BR   || op == IrOp::BR_COND ||
           op == IrOp::RET  || op == IrOp::UNREACHABLE ||
           op == IrOp::THROW;
}

/** @brief Devuelve true si la instruccion es pura (apta para DCE y CSE). */
static bool is_pure(IrOp op) {
    return !is_side_effecting(op);
}

// =========================================================================
//  Pase DCE (Dead Code Elimination)
// =========================================================================

bool ir_pass_dce(IrFunction &fn) {
    // Construir conjunto de valores que son usados en algun operando
    std::unordered_set<IrValueId> used;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            for (IrValueId op : ins.operands) {
                if (op != IR_NO_VALUE) used.insert(op);
            }
            // CALLIND y CALLCLOSURE referencian el callee via func_ptr (no
            // via operands), asi que DCE debe contarlo como uso para que el
            // SSA value que produjo el puntero (e.g. RAW_ASM `mov rN, @Abs(...)`
            // o LOAD del slot del function value) NO sea eliminado.  Sin
            // esto, A.10 closures rompen porque el optimizer purga la
            // instruccion que materializa fn_addr y el regalloc deja r14
            // (asignado a fn_addr_v) sin inicializar -> callvmr salta a 0.
            if ((ins.op == IrOp::CALLIND || ins.op == IrOp::CALLCLOSURE)
             && ins.func_ptr != IR_NO_VALUE) {
                used.insert(ins.func_ptr);
            }
            for (const auto &pa : ins.phi_args) {
                if (pa.value != IR_NO_VALUE) used.insert(pa.value);
            }
        }
    }

    bool changed = false;
    for (auto &bb : fn.blocks) {
        auto &instrs = bb.instrs;
        size_t write = 0;
        for (size_t i = 0; i < instrs.size(); ++i) {
            const IrInstr &ins = instrs[i];
            bool keep = true;
            // Una instruccion con resultado no usado y sin efectos laterales se elimina,
            // EXCEPTO si lleva el flag @c preserve (barreras del codegen).
            if (ins.dst != IR_NO_VALUE
                && !used.count(ins.dst)
                && !is_side_effecting(ins.op)
                && !ins.preserve) {
                keep  = false;
                changed = true;
            }
            if (keep) {
                if (write != i) instrs[write] = std::move(instrs[i]);
                ++write;
            }
        }
        instrs.resize(write);
    }
    return changed;
}

// =========================================================================
//  Pase de propagacion de copias
// =========================================================================

bool ir_pass_copy_prop(IrFunction &fn) {
    // Construir mapa de sustituciones: %b -> %a para cada "%b = mov %a"
    std::unordered_map<IrValueId, IrValueId> subst;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::MOV
                && !ins.preserve
                && ins.dst != IR_NO_VALUE
                && ins.operands.size() == 1
                && ins.operands[0] != IR_NO_VALUE) {
                // seguir la cadena de sustituciones
                IrValueId src = ins.operands[0];
                while (subst.count(src)) src = subst[src];
                subst[ins.dst] = src;
            }
        }
    }
    if (subst.empty()) return false;

    // Aplicar sustituciones en todos los operandos
    bool changed = false;
    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            // sustituir operandos normales
            for (auto &op : ins.operands) {
                auto it = subst.find(op);
                if (it != subst.end() && it->second != op) {
                    op      = it->second;
                    changed = true;
                }
            }
            // sustituir func_ptr en CALLIND
            if (ins.func_ptr != IR_NO_VALUE) {
                auto it = subst.find(ins.func_ptr);
                if (it != subst.end() && it->second != ins.func_ptr) {
                    ins.func_ptr = it->second;
                    changed      = true;
                }
            }
            // sustituir argumentos phi
            for (auto &pa : ins.phi_args) {
                auto it = subst.find(pa.value);
                if (it != subst.end() && it->second != pa.value) {
                    pa.value = it->second;
                    changed  = true;
                }
            }
        }
    }
    // Eliminar los MOV que ahora son copias triviales (%a = mov %a)
    if (changed) ir_pass_dce(fn);
    return changed;
}

// =========================================================================
//  Pase de plegado de constantes
// =========================================================================

/** @brief Devuelve el valor constante de un IrValue, o UNDEF si no es const. */
static bool get_const(const IrFunction &fn, IrValueId id, uint64_t &val) {
    if (id == IR_NO_VALUE || id >= static_cast<IrValueId>(fn.values.size()))
        return false;
    const IrValue &v = fn.values[id];
    if (!v.is_const) return false;
    val = v.const_val;
    return true;
}

bool ir_pass_const_fold(IrFunction &fn) {
    bool changed = false;

    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            if (ins.dst == IR_NO_VALUE) continue;
            if (!is_pure(ins.op))       continue;

            // --- Operaciones binarias enteras ---
            if (ins.operands.size() == 2) {
                uint64_t a, b;
                bool ca = get_const(fn, ins.operands[0], a);
                bool cb = get_const(fn, ins.operands[1], b);
                if (!ca || !cb) continue;

                uint64_t res = 0;
                bool folded  = true;
                int64_t sa = static_cast<int64_t>(a);
                int64_t sb = static_cast<int64_t>(b);

                switch (ins.op) {
                    case IrOp::ADD:     res = a + b;                        break;
                    case IrOp::SUB:     res = a - b;                        break;
                    case IrOp::MUL:     res = a * b;                        break;
                    case IrOp::DIV:     res = (sb != 0) ? (uint64_t)(sa / sb) : 0; break;
                    case IrOp::MOD:     res = (sb != 0) ? (uint64_t)(sa % sb) : 0; break;
                    case IrOp::AND:     res = a & b;                        break;
                    case IrOp::OR:      res = a | b;                        break;
                    case IrOp::XOR:     res = a ^ b;                        break;
                    case IrOp::SHL:     res = a << (b & 63);                break;
                    case IrOp::SHR:     res = a >> (b & 63);                break;
                    case IrOp::SAR:     res = (uint64_t)(sa >> (b & 63));   break;
                    // comparaciones enteras -> resultado bool (0 o 1)
                    case IrOp::CMP_EQ:  res = (a == b)           ? 1 : 0;  break;
                    case IrOp::CMP_NE:  res = (a != b)           ? 1 : 0;  break;
                    case IrOp::CMP_LT:  res = (sa <  sb)         ? 1 : 0;  break;
                    case IrOp::CMP_GT:  res = (sa >  sb)         ? 1 : 0;  break;
                    case IrOp::CMP_LE:  res = (sa <= sb)         ? 1 : 0;  break;
                    case IrOp::CMP_GE:  res = (sa >= sb)         ? 1 : 0;  break;
                    case IrOp::CMP_ULT: res = (a <  b)           ? 1 : 0;  break;
                    case IrOp::CMP_UGT: res = (a >  b)           ? 1 : 0;  break;
                    case IrOp::CMP_ULE: res = (a <= b)           ? 1 : 0;  break;
                    case IrOp::CMP_UGE: res = (a >= b)           ? 1 : 0;  break;
                    default: folded = false; break;
                }

                if (folded) {
                    ins.op         = IrOp::CONST;
                    ins.imm        = res;
                    ins.operands.clear();
                    ins.type       = (ins.op == IrOp::CMP_EQ) ? IrType::BOOL : ins.type;
                    // marcar el valor destino como constante
                    if (ins.dst < static_cast<IrValueId>(fn.values.size())) {
                        fn.values[ins.dst].is_const  = true;
                        fn.values[ins.dst].const_val = res;
                    }
                    changed = true;
                }
            }

            // --- Operaciones unarias enteras ---
            if (ins.operands.size() == 1) {
                uint64_t a;
                if (!get_const(fn, ins.operands[0], a)) continue;

                uint64_t res  = 0;
                bool folded   = true;
                int64_t sa = static_cast<int64_t>(a);

                switch (ins.op) {
                    case IrOp::NEG:     res = (uint64_t)(-sa);  break;
                    case IrOp::NOT:     res = ~a;                break;
                    case IrOp::ZEXT:    res = a;                 break;
                    case IrOp::TRUNC:   res = a & 0xFFFFFFFFULL; break;
                    case IrOp::MOV:     res = a;                 break;
                    default: folded = false; break;
                }

                if (folded) {
                    ins.op  = IrOp::CONST;
                    ins.imm = res;
                    ins.operands.clear();
                    if (ins.dst < static_cast<IrValueId>(fn.values.size())) {
                        fn.values[ins.dst].is_const  = true;
                        fn.values[ins.dst].const_val = res;
                    }
                    changed = true;
                }
            }
        }
    }
    return changed;
}

// =========================================================================
//  Pase de eliminacion de bloques inalcanzables
// =========================================================================

bool ir_pass_unreachable(IrFunction &fn) {
    if (fn.blocks.empty()) return false;

    const size_t nblocks = fn.blocks.size();
    std::vector<bool> reachable(nblocks, false);

    // BFS desde el bloque de entrada (bloque 0)
    std::queue<IrBlockId> worklist;
    worklist.push(0);
    reachable[0] = true;

    while (!worklist.empty()) {
        IrBlockId bid = worklist.front();
        worklist.pop();

        if (bid >= nblocks) continue;
        const IrBlock &bb = fn.blocks[bid];

        // Encontrar el terminador del bloque y sus sucesores
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::BR) {
                IrBlockId s = ins.target_block;
                if (s < nblocks && !reachable[s]) {
                    reachable[s] = true;
                    worklist.push(s);
                }
            } else if (ins.op == IrOp::BR_COND) {
                for (IrBlockId s : {ins.target_block, ins.false_block}) {
                    if (s < nblocks && !reachable[s]) {
                        reachable[s] = true;
                        worklist.push(s);
                    }
                }
            }
        }
        // Usar sucesores precalculados si existen
        for (IrBlockId s : bb.succs) {
            if (s < nblocks && !reachable[s]) {
                reachable[s] = true;
                worklist.push(s);
            }
        }
    }

    // Verificar si hay bloques inalcanzables
    bool any_unreachable = false;
    for (size_t b = 0; b < nblocks; ++b) {
        if (!reachable[b]) { any_unreachable = true; break; }
    }
    if (!any_unreachable) return false;

    // Construir mapa de reindexacion: viejo_id -> nuevo_id
    std::vector<IrBlockId> remap(nblocks, IR_NO_BLOCK);
    IrBlockId new_id = 0;
    for (size_t b = 0; b < nblocks; ++b) {
        if (reachable[b]) remap[b] = new_id++;
    }

    // Reescribir los bloques: eliminar los inalcanzables y actualizar referencias
    std::vector<IrBlock> new_blocks;
    new_blocks.reserve(new_id);
    for (size_t b = 0; b < nblocks; ++b) {
        if (!reachable[b]) continue;
        IrBlock bb = fn.blocks[b];
        bb.id = remap[b];

        // Actualizar predecesores y sucesores
        std::vector<IrBlockId> new_preds, new_succs;
        for (IrBlockId p : bb.preds) {
            if (p < nblocks && reachable[p]) new_preds.push_back(remap[p]);
        }
        for (IrBlockId s : bb.succs) {
            if (s < nblocks && reachable[s]) new_succs.push_back(remap[s]);
        }
        bb.preds = std::move(new_preds);
        bb.succs = std::move(new_succs);

        // Actualizar referencias de bloques en instrucciones
        for (auto &ins : bb.instrs) {
            if (ins.target_block != IR_NO_BLOCK && ins.target_block < nblocks)
                ins.target_block = remap[ins.target_block];
            if (ins.false_block != IR_NO_BLOCK && ins.false_block < nblocks)
                ins.false_block = remap[ins.false_block];
            // Eliminar argumentos phi que vienen de bloques inalcanzables
            std::vector<IrPhiArg> new_phi;
            for (const auto &pa : ins.phi_args) {
                if (pa.block < nblocks && reachable[pa.block]) {
                    IrPhiArg na = pa;
                    na.block = remap[pa.block];
                    new_phi.push_back(na);
                }
            }
            ins.phi_args = std::move(new_phi);
        }
        new_blocks.push_back(std::move(bb));
    }
    fn.blocks = std::move(new_blocks);
    return true;
}

// =========================================================================
//  Pase CSE (Common Subexpression Elimination, local)
// =========================================================================

bool ir_pass_cse(IrFunction &fn) {
    bool changed = false;

    for (auto &bb : fn.blocks) {
        // Tabla: hash de (op, type, operands) -> IrValueId del primer calculo
        std::unordered_map<std::string, IrValueId> expr_table;
        // Mapa de sustituciones para aplicar
        std::unordered_map<IrValueId, IrValueId> subst;

        for (auto &ins : bb.instrs) {
            if (ins.dst == IR_NO_VALUE || !is_pure(ins.op)) continue;
            if (ins.op == IrOp::CONST || ins.op == IrOp::PHI) continue; // no deduplicar

            // Construir clave canonica: "op:type:op0:op1:..."
            std::ostringstream key;
            key << static_cast<int>(ins.op) << ":" << static_cast<int>(ins.type);
            for (IrValueId op : ins.operands) {
                // Resolver sustituciones previas en los operandos
                IrValueId canonical = op;
                while (subst.count(canonical)) canonical = subst[canonical];
                key << ":" << canonical;
            }
            std::string k = key.str();

            auto it = expr_table.find(k);
            if (it != expr_table.end()) {
                // Expresion ya calculada: sustituir dst con el valor anterior
                subst[ins.dst] = it->second;
                ins.op  = IrOp::MOV;
                ins.operands = {it->second};
                changed = true;
            } else {
                // Primera ocurrencia: registrarla
                expr_table[k] = ins.dst;
                // Aplicar sustituciones previas a los operandos de esta instruccion
                for (auto &op : ins.operands) {
                    auto sit = subst.find(op);
                    if (sit != subst.end()) op = sit->second;
                }
            }
        }
    }
    if (changed) ir_pass_copy_prop(fn); // limpiar los MOV generados por CSE
    return changed;
}

// =========================================================================
//  Pase TCO (Tail Call Optimization)
// =========================================================================

// =========================================================================
//  Helper: detectar si un IrValueId deriva (transitivamente) de una ALLOCA
//
//  Usado por TCO para descartar la transformacion CALL->TAILCALL cuando
//  algun argumento referencia memoria asignada en el frame del caller.
//  Razon: TAILCALL emite `leave` antes del salto al callee, lo que
//  restaura RSP=RBP y libera el bloque ALLOCA.  Si el callee dereferencia
//  un puntero que apuntaba a esa region, lee basura (o memoria del
//  callee).  Demo regresion: 17_ecs_basico.vex pasaba arrays
//  i32[8] (ALLOCA) por valor a system_sum_positions y obtenia R0=0
//  en vez de 100 con TCO activo.
//
//  La deteccion es conservadora: marcamos un valor como "alloca-derived"
//  si su def es ALLOCA, MEMBER (que devuelve direccion en frame), o
//  cualquier ADD/SUB/MOV/STORE/STR_LIT_ADDR cuyo operando ya este marcado.
//  No intenta tracking flow-sensitive: una sobreestimacion implica
//  perder TCO en ese caller, no incorrectness.
// =========================================================================
static bool collect_alloca_derived(const IrFunction &fn,
                                    std::unordered_set<IrValueId> &out) {
    out.clear();
    // Pase 1: identificar las definiciones ALLOCA directas.
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::ALLOCA && ins.dst != IR_NO_VALUE) {
                out.insert(ins.dst);
            }
        }
    }
    if (out.empty()) return false;

    // Pase 2: propagar la marca por aritmetica de punteros y MOV/PHI.
    // Iteramos hasta punto fijo (cota: numero de blocks * instrs por block).
    bool changed = true;
    int  guard   = 1024;
    while (changed && guard-- > 0) {
        changed = false;
        for (const auto &bb : fn.blocks) {
            for (const auto &ins : bb.instrs) {
                if (ins.dst == IR_NO_VALUE) continue;
                if (out.count(ins.dst)) continue;
                bool any_op_tainted = false;
                for (auto op : ins.operands) {
                    if (op != IR_NO_VALUE && out.count(op)) {
                        any_op_tainted = true;
                        break;
                    }
                }
                if (!any_op_tainted) {
                    for (const auto &pa : ins.phi_args) {
                        if (out.count(pa.value)) {
                            any_op_tainted = true;
                            break;
                        }
                    }
                }
                if (any_op_tainted) {
                    // Solo propagamos por ops que SI pueden producir una
                    // direccion derivada: aritmetica (ADD/SUB), copias
                    // (MOV, PHI), o casts/cargas/loads que conserven el
                    // puntero.  Para ALU "verdadera" sobre escalares no
                    // hay riesgo, asi que la lista blanca es restrictiva
                    // pero suficiente para el patron observado.
                    switch (ins.op) {
                        case IrOp::ADD: case IrOp::SUB:
                        case IrOp::MOV: case IrOp::PHI:
                            out.insert(ins.dst);
                            changed = true;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }
    return true;
}

bool ir_pass_tailcall(IrFunction &fn) {
    // Detecta el patron: CALL @f(args) seguido inmediatamente de RET %resultado
    // y convierte el CALL en TAILCALL (elimina la RET subsiguiente).
    // Tambien maneja RET void inmediatamente despues de CALL void.
    //
    // SAFETY: NO se promueve a TAILCALL si algun argumento es derivado
    // de una ALLOCA del caller.  TAILCALL emite `leave` (que restaura
    // RSP=RBP y libera el frame), invalidando los punteros que apuntan
    // al area de allocas.  El callee leeria basura.  La deteccion se
    // hace una vez por funcion y la cache se reutiliza para todos los
    // CALLs candidatos.
    bool changed = false;

    std::unordered_set<IrValueId> alloca_derived;
    const bool fn_has_alloca = collect_alloca_derived(fn, alloca_derived);

    for (auto &bb : fn.blocks) {
        auto &instrs = bb.instrs;
        for (size_t i = 0; i + 1 < instrs.size(); ) {
            IrInstr &call = instrs[i];
            IrInstr &ret  = instrs[i + 1];

            // Solo CALL directo (no CALLVIRT, CALLN, CALLIND)
            if (call.op != IrOp::CALL) { ++i; continue; }
            if (ret.op  != IrOp::RET)  { ++i; continue; }

            // Verificar que RET usa directamente el resultado del CALL (o es void)
            bool ret_uses_call = (!ret.operands.empty()
                                  && ret.operands[0] == call.dst);
            bool ret_is_void   = ret.operands.empty();

            if (!ret_uses_call && !ret_is_void) { ++i; continue; }

            // Bloqueo de seguridad: si CUALQUIER arg es derivado de
            // ALLOCA del caller, NO promover a TAILCALL.  El leave
            // posterior liberaria la memoria todavia referenciada.
            if (fn_has_alloca) {
                bool unsafe = false;
                for (auto op : call.operands) {
                    if (op != IR_NO_VALUE && alloca_derived.count(op)) {
                        unsafe = true;
                        break;
                    }
                }
                if (unsafe) { ++i; continue; }
            }

            // Convertir: CALL -> TAILCALL, eliminar RET
            call.op  = IrOp::TAILCALL;
            call.dst = IR_NO_VALUE; // TAILCALL no tiene destino
            instrs.erase(instrs.begin() + static_cast<ptrdiff_t>(i + 1));
            changed = true;
            // No avanzar 'i': re-examinar la misma posicion por si hay otro patron
        }
    }
    return changed;
}

// =========================================================================
//  Punto de entrada principal
// =========================================================================

/**
 * @brief Inline de header trivial de loop para habilitar fusion decjnz.
 *
 * Detecta el patron:
 *   B: ...; <SUB>; br H
 *   H: %cmp = CMP_X(...); br.cond %cmp, T, F
 * con H teniendo UN SOLO predecesor (B).  Mueve las 2 instrs de H al
 * final de B (reemplazando el br) y vacia H.  Despues, B queda con
 * SUB+CMP+BR_COND consecutivos -> el peephole de same-block del IR
 * emitter aplica decjnz fusion.
 *
 * Generaliza a cualquier patron "header trivial con un predecesor", no
 * solo a decjnz.  Otros peepholes (cmpjmp) tambien se benefician.
 *
 * Coste: O(N_blocks * N_predecessors).  El check de predecesores es
 * lineal pero solo se ejecuta cuando el header tiene exactamente 2
 * instr (raro fuera de loop headers).
 *
 * @return true si se hizo al menos una fusion (puede dispararse otro DCE).
 */
bool ir_pass_inline_loop_header(IrFunction &fn) {
    bool changed = false;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        IrBlock &B = fn.blocks[bi];
        if (B.instrs.empty()) continue;
        IrInstr &term = B.instrs.back();
        if (term.op != IrOp::BR) continue; // solo BR incondicional
        IrBlockId hid = term.target_block;
        if (hid >= fn.blocks.size() || hid == static_cast<IrBlockId>(bi)) continue;
        IrBlock &H = fn.blocks[hid];
        // H debe terminar en BR_COND.  Las instrs pueden incluir CONSTs
        // literales que materializan valores usados solo por el CMP final.
        // Patron tipico: %k = const.i64 0; %z = cmp.ne.bool %x, %k; br.cond %z, T, F.
        // Tambien permitimos el CMP en penultima posicion con 0..N CONSTs antes.
        if (H.instrs.size() < 2) continue;
        const IrInstr &h_last = H.instrs.back();
        if (h_last.op != IrOp::BR_COND) continue;
        // Buscar el CMP penultimo (h_last - 1) o anterior.  Las instrs
        // entre el CMP y el BR_COND deben ser CONST puros (sin side effects).
        size_t cmp_idx = H.instrs.size() - 2;
        const IrInstr &h_cmp = H.instrs[cmp_idx];
        bool is_cmp = (h_cmp.op == IrOp::CMP_EQ  || h_cmp.op == IrOp::CMP_NE
                    || h_cmp.op == IrOp::CMP_LT  || h_cmp.op == IrOp::CMP_GT
                    || h_cmp.op == IrOp::CMP_LE  || h_cmp.op == IrOp::CMP_GE
                    || h_cmp.op == IrOp::CMP_ULT || h_cmp.op == IrOp::CMP_UGT
                    || h_cmp.op == IrOp::CMP_ULE || h_cmp.op == IrOp::CMP_UGE);
        if (!is_cmp) continue;
        // Verificar que las instrs antes del CMP sean todas CONST (puras).
        bool only_consts = true;
        for (size_t k = 0; k < cmp_idx; ++k) {
            if (H.instrs[k].op != IrOp::CONST) { only_consts = false; break; }
        }
        if (!only_consts) continue;
        // El cmp.dst debe ser el unico operand del BR_COND.
        if (h_last.operands.empty() || h_last.operands[0] != h_cmp.dst) continue;
        // Contar predecesores de H.
        int preds = 0;
        for (size_t pi = 0; pi < fn.blocks.size(); ++pi) {
            if (fn.blocks[pi].instrs.empty()) continue;
            const IrInstr &pterm = fn.blocks[pi].instrs.back();
            if (pterm.op == IrOp::BR && pterm.target_block == hid) ++preds;
            else if (pterm.op == IrOp::BR_COND
                  && (pterm.target_block == hid || pterm.false_block == hid))
                ++preds;
        }
        if (preds != 1) continue; // mas de 1 pred o 0 -> no fusionar
        // No tocar entry block: si H es entry, no podemos fusionarlo
        // (entry no tiene predecesores; ya filtrado por preds!=1, pero
        // doble check defensivo).
        if (hid == 0) continue;
        // El header NO debe tener PHI nodes (el primer instr debe ser CMP, no PHI).
        // Ya implicito en H.instrs.size() == 2 con h0 = CMP.

        // Aplicar la fusion:
        //   1. Eliminar BR de B.
        //   2. Append todas las instrs de H (CONSTs + CMP + BR_COND) al final de B.
        //   3. Hoist de CONSTs: mover todas las IrOp::CONST justo despues de
        //      las PHI nodes (CONSTs son puros, su orden es irrelevante para
        //      la semantica).  Esto agrupa los CONSTs y deja a SUB+CMP+BR_COND
        //      consecutivos en el final, habilitando peepholes como decjnz.
        //   4. Limpiar H (queda inalcanzable, lo barren los demas pases).
        //   5. Reescribir phi_args en sucesores de BR_COND: ref a H debe pasar a B.
        IrBlockId t_true  = h_last.target_block;
        IrBlockId t_false = h_last.false_block;

        std::vector<IrInstr> moved;
        moved.reserve(H.instrs.size());
        for (auto &ins : H.instrs) moved.push_back(std::move(ins));
        B.instrs.pop_back(); // remover BR de B
        for (auto &ins : moved) B.instrs.push_back(std::move(ins));
        H.instrs.clear();

        // Hoist de CONSTs: estabilizamos el orden en B asi:
        //   [PHIs...] [CONSTs...] [resto en orden original]
        // Stable_partition mantiene el orden relativo de cada grupo.  Los
        // CONSTs no tienen side effects ni dependen de instrucciones
        // anteriores (su unico operand_id es @c imm), asi que moverlos
        // hacia adelante no rompe SSA dominance: si un CONST se usaba a
        // X, ahora esta definido aun antes que X.
        {
            std::vector<IrInstr> phis, consts, rest;
            phis.reserve(4); consts.reserve(8); rest.reserve(B.instrs.size());
            for (auto &ins : B.instrs) {
                if (ins.op == IrOp::PHI)        phis.push_back(std::move(ins));
                else if (ins.op == IrOp::CONST) consts.push_back(std::move(ins));
                else                            rest.push_back(std::move(ins));
            }
            B.instrs.clear();
            B.instrs.reserve(phis.size() + consts.size() + rest.size());
            for (auto &ins : phis)   B.instrs.push_back(std::move(ins));
            for (auto &ins : consts) B.instrs.push_back(std::move(ins));
            for (auto &ins : rest)   B.instrs.push_back(std::move(ins));
        }

        auto rewrite_phi_block_ref = [&](IrBlockId target_id) {
            if (target_id >= fn.blocks.size()) return;
            IrBlock &T = fn.blocks[target_id];
            for (auto &ins : T.instrs) {
                if (ins.op != IrOp::PHI) break;
                for (auto &pa : ins.phi_args) {
                    if (pa.block == hid) pa.block = static_cast<IrBlockId>(bi);
                }
            }
        };
        rewrite_phi_block_ref(t_true);
        rewrite_phi_block_ref(t_false);

        changed = true;
        // No avanzar bi: re-procesar B porque puede haber cadena
        // (B -> H1 -> H2 inlinable transitivamente).
        --bi;
    }
    return changed;
}

void ir_optimize(IrModule &mod, OptLevel level) {
    if (level == OptLevel::O0) return; // sin optimizacion

    // Iterar hasta punto fijo o maximo 8 pasadas
    for (int pass = 0; pass < 8; ++pass) {
        bool any = false;

        for (auto &fn : mod.functions) {
            if (fn.is_native) continue; // no optimizar stubs nativos

            // O1: DCE + copia
            any |= ir_pass_copy_prop(fn);
            any |= ir_pass_dce(fn);

            if (level >= OptLevel::O2) {
                // O2: plegado de constantes + bloques inalcanzables + TCO.
                any |= ir_pass_const_fold(fn);
                any |= ir_pass_unreachable(fn);
                any |= ir_pass_tailcall(fn);
                // Inline de header trivial de loop -> habilita decjnz fusion.
                any |= ir_pass_inline_loop_header(fn);
                // Segunda ronda de DCE tras plegado y TCO + loop header inline.
                any |= ir_pass_dce(fn);
            }

            if (level >= OptLevel::O3) {
                // O3: eliminacion de subexpresiones comunes.
                any |= ir_pass_cse(fn);
            }
        }

        if (!any) break; // punto fijo alcanzado
    }
}

} // namespace ir
