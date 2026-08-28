/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/builtins_concurrent.cpp
 * @brief Bajada de los builtins que hablan CON OTRO: memoria compartida,
 *        operaciones atomicas, buzones y futuros.
 *
 * Lo que une a estos trece no es lo que hacen sino con QUIEN: todos suponen
 * que hay alguien mas -- otro proceso, otro hilo -- y su razon de ser es que
 * lo que uno escribe lo vea el otro, y en el orden correcto.  De ahi que
 * `share` no sea reservar memoria (eso lo hace `malloc`) sino sacarla del
 * monton privado del proceso y ponerla donde el vecino pueda leerla, y que un
 * `atomic_add` no sea sumar (eso es `+`) sino sumar SIN que el vecino vea el
 * estado a medias.
 *
 * Los futuros y los buzones son la misma idea a otra escala: en vez de
 * compartir bytes se comparte un aviso.  `msgsend` deja un mensaje y sigue;
 * `future_alloc` reserva la casilla de una respuesta que todavia no existe, y
 * `fulfill` la rellena cuando llega, despertando a quien la esperaba.
 *
 * Todos bajan a instrucciones que la maquina virtual ya tiene, asi que aqui no
 * hay logica de sincronizacion: solo se traduce la llamada a la instruccion, y
 * se comprueba que los argumentos tienen sentido antes.
 *
 * Se separo de la funcion que despacha todos los builtins.  Entra por su
 * propio punto: si el nombre no es de esta familia, contesta que no y quien
 * pregunta sigue con las demas.
 */
#include "util/env_flags.h"
#include "vx/lowering.h"
#include "ir/ir_type_info.h" // vocabulario UNICO de anchura/clase de un IrType
#include "loader/oop_types.h" // ADVICE_*: el orden de la cadena
#include <algorithm>
#include <chrono>
#include <iostream>
#include "ffi/virtual_lib_registry.h" // lookup_virtual_fn (bug 161: MC.23)
#include "vx/asm/asm_effects.h"       // inferencia de clobbers ( AS inc.4)
#include "vx/asm/asm_diag.h"      // diagnosticos estructurales del asm (ASA.2)
#include "vx/asm/asm_lift_emit.h" // lift de patrones atomicos a IR tipado (ASA.3)
#include "vx/asm/asm_lift_general.h" // lift general straight-line entero a IR real
#include "vx/asm/asm_lift_micro.h"
#include "vx/asm/asm_lift_registro.h"
#include "vx/asm/asm_phys_reg.h" // asm_body_subst_greedy // lift de asm opaco sin operandos -> ASM_MICRO
#include "vx/asm/instr_db.h"    // reschedule_asm (reoptimizador de asm, ASA)
#include "vx/asm/asm_backend.h" // validacion de sintaxis via Keystone (inc.4b)
#include "vx/collection_intrinsics.h"        // tabla de tipos coleccion
#include "vx/comptime/comptime_introspect.h" // helpers compartidos rama A
#include "vx/generics/concepts.h"      // conceptos como predicado -> CONST bool
#include "vx/generics/generic_clone.h" // clone_expr (custom print to_string)
#include "vx/lexer.h"                  // parse de fragments para @Macro
#include "vx/parser.h"                 // parse_one_expr para @Macro
#include "ir/ir_optimizer.h"           // register_pure_new_helper
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include "lowering_internal.h" // la cocina compartida del lowering (no es su interfaz)

namespace vx {

/**
 * @brief Intenta bajar @p e como uno de los builtins de concurrencia.
 *
 * @param e         La llamada.
 * @param b         Que builtin es, ya resuelto por quien despacha.
 * @param out_value Donde dejar el resultado; sin valor si el builtin no lo da.
 * @return @c true si @p b era de esta familia y quedo bajado.
 */
bool Lowering::try_lower_concurrent_builtins(ast::CallExpr *e,
                                             Builtin b,
                                             ir::IrValueId &out_value) {
    // Memoria compartida entre procesos: sacarla del monton privado y
    // devolverla, mas saber si una direccion dada ya esta compartida.
    const bool is_z6_isshared = (b == Builtin::IsShared);
    const bool is_z6_share = (b == Builtin::Share);
    const bool is_z6_unshare = (b == Builtin::Unshare);
    // Operaciones atomicas sobre esa memoria: leer/escribir sin ver estados a
    // medias, y las dos que hacen falta para construir cualquier otra cosa.
    const bool is_z8_atomic_load = (b == Builtin::AtomicLoadI64);
    const bool is_z8_atomic_store = (b == Builtin::AtomicStoreI64);
    const bool is_z8_atomic_cas = (b == Builtin::AtomicCasI64);
    const bool is_z8_atomic_add = (b == Builtin::AtomicAddI64);
    const bool is_z8_shared_malloc = (b == Builtin::SharedMalloc);
    const bool is_z8_shared_free = (b == Builtin::SharedFree);
    // Buzones: dejar un mensaje a otro proceso y recogerlo del propio.
    const bool is_msgsend = (b == Builtin::Msgsend);
    const bool is_msgrecv = (b == Builtin::Msgrecv);
    // Futuros: la casilla de una respuesta que aun no existe, y rellenarla.
    const bool is_future_alloc = (b == Builtin::FutureAlloc);
    const bool is_fulfill = (b == Builtin::Fulfill);

    /* Salida rapida: si no es de esta familia no se mira nada de lo de abajo. */
    if (!(is_z6_isshared || is_z6_share || is_z6_unshare ||
          is_z8_atomic_load || is_z8_atomic_store || is_z8_atomic_cas ||
          is_z8_atomic_add || is_z8_shared_malloc || is_z8_shared_free ||
          is_msgsend || is_msgrecv || is_future_alloc || is_fulfill))
        return false;

    if (is_z6_isshared) {
        if (e->args.size() != 1) {
            error_at(e->loc, "is_shared: requiere 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_obj = lower_expr(e->args[0].get());
        if (v_obj == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // 1) gchandle obj -> handle (u64)
        const ir::IrValueId v_h = emit_gc_handle_for_ptr(v_obj, e->loc.line);
        // 2) const mask
        const ir::IrValueId v_mask =
            emit_const(ir::IrType::U64, 0x80000000ULL, e->loc.line);
        // 3) handle & mask
        const ir::IrValueId v_and = fn_->new_value(ir::IrType::U64);
        {
            ir::IrInstr ins{};
            ins.op = ir::IrOp::AND;
            ins.type = ir::IrType::U64;
            ins.dst = v_and;
            ins.operands = {v_h, v_mask};
            ins.source_line = e->loc.line;
            emit(current_block_, std::move(ins));
        }
        // 4) shr 31 -> bit 0 = 0|1
        const ir::IrValueId v_shift =
            emit_const(ir::IrType::U64, 31ULL, e->loc.line);
        const ir::IrValueId v_bit = fn_->new_value(ir::IrType::U64);
        {
            ir::IrInstr ins{};
            ins.op = ir::IrOp::SHR;
            ins.type = ir::IrType::U64;
            ins.dst = v_bit;
            ins.operands = {v_and, v_shift};
            ins.source_line = e->loc.line;
            emit(current_block_, std::move(ins));
        }
        // 5) cast a bool
        const ir::IrValueId v_bool =
            cast_if_needed(v_bit, ir::IrType::U64, ir::IrType::BOOL,
                           e->loc.line, /*explicit=*/true);
        out_value = v_bool;
        return true;
    }

    // ----- Z.7: share(obj) -> obj (in-place promotion via gcpromote) -----
    // Emite el opcode bytecode @c gcpromote (extended 0xA7) que aloca
    // en el SharedHeap, copia el objeto, registra en SharedHandleTable
    // y devuelve el nuevo host_ptr.  Si el objeto ya esta shared
    // (bit 31 en hash_code), el opcode es no-op idempotente.
    //
    // NOTA: el resultado de share() es una NUEVA referencia.  El
    // original sigue en el gc_heap local (se libera por sweep).
    // Patron correcto en Vesta:
    //   Counter c = new Counter();
    //   c = share(c);                  // c ahora apunta a la copia shared
    if (is_z6_share) {
        if (e->args.size() != 1) {
            error_at(e->loc, "share: requiere 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_obj = lower_expr(e->args[0].get());
        if (v_obj == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_new = emit_gc_promote(v_obj, e->loc.line);
        fn_->values[v_new].is_gc_object =
            true; // marca de host_ptr a payload GC
        out_value = v_new;
        return true;
    }

    // ----- Z.7: unshare(obj) -> obj (deep copy a heap local) -----
    // Emite @c gcdemote (extended 0xA8).  Idempotente si NO es shared.
    if (is_z6_unshare) {
        if (e->args.size() != 1) {
            error_at(e->loc, "unshare: requiere 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_obj = lower_expr(e->args[0].get());
        if (v_obj == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_new = emit_gc_demote(v_obj, e->loc.line);
        fn_->values[v_new].is_gc_object = true;
        out_value = v_new;
        return true;
    }

    // ============================================================
    // Z.8 atomic primitives: bajan a opcodes atomicld/st/cas/add.
    // El host_ptr es un i64 que se interpreta como direccion
    // absoluta del host (no VM addr).  El usuario es responsable
    // de la alineacion a 8 bytes para lock-free.
    // ============================================================

    // atomic_load_i64(ptr) -> i64
    if (is_z8_atomic_load) {
        if (e->args.size() != 1) {
            error_at(e->loc, "atomic_load_i64: requiere 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
        out_value = emit_atomic_ld_i64(v_ptr, e->loc.line);
        return true;
    }

    // atomic_store_i64(ptr, val) -> void
    if (is_z8_atomic_store) {
        if (e->args.size() != 2) {
            error_at(e->loc, "atomic_store_i64: requiere 2 argumentos");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
        const ir::IrValueId v_val = lower_expr(e->args[1].get());
        emit_atomic_st_i64(v_ptr, v_val, e->loc.line);
        out_value = ir::IR_NO_VALUE;
        return true;
    }

    // atomic_cas_i64(ptr, exp, des) -> i64 (old value)
    if (is_z8_atomic_cas) {
        if (e->args.size() != 3) {
            error_at(e->loc, "atomic_cas_i64: requiere 3 argumentos");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
        const ir::IrValueId v_exp = lower_expr(e->args[1].get());
        const ir::IrValueId v_des = lower_expr(e->args[2].get());
        out_value = emit_atomic_cas_i64(v_ptr, v_exp, v_des, e->loc.line);
        return true;
    }

    // atomic_add_i64(ptr, delta) -> i64 (old value)
    if (is_z8_atomic_add) {
        if (e->args.size() != 2) {
            error_at(e->loc, "atomic_add_i64: requiere 2 argumentos");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
        const ir::IrValueId v_delta = lower_expr(e->args[1].get());
        out_value = emit_atomic_add_i64(v_ptr, v_delta, e->loc.line);
        return true;
    }

    // shared_malloc(size) -> i64* (host_ptr).  Implementacion via CALLN
    // a un wrapper que usa @c vm.shared_heap.alloc.  Por ahora
    // delegamos al @c malloc estandar (raw_allocator) que tambien da
    // memoria host accesible cross-process (mismo address space).
    if (is_z8_shared_malloc) {
        if (e->args.size() != 1) {
            error_at(e->loc, "shared_malloc: requiere 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_size = lower_expr(e->args[0].get());
        const ir::IrValueId v_ptr = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_ptr].is_host_ptr = true;
        // Reuse @c alloc opcode (RAW_ALLOC IR op).  La memoria que
        // retorna es host_ptr, identico en todos los procesos (mismo
        // address space del OS).  Para promocion al SharedHeap real
        // (con tracking de GC), usar @c share() en el objeto creado.
        ir::IrInstr ins{};
        ins.op = ir::IrOp::RAW_ALLOC;
        ins.type = ir::IrType::PTR;
        ins.dst = v_ptr;
        ins.operands = {v_size};
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        out_value = v_ptr;
        return true;
    }

    // shared_free(ptr) -> void
    if (is_z8_shared_free) {
        if (e->args.size() != 1) {
            error_at(e->loc, "shared_free: requiere 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
        ir::IrInstr ins{};
        ins.op = ir::IrOp::RAW_FREE;
        ins.type = ir::IrType::VOID;
        ins.dst = ir::IR_NO_VALUE;
        ins.operands = {v_ptr};
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        out_value = ir::IR_NO_VALUE;
        return true;
    }

    if (is_msgsend) {
        if (e->args.size() != 2) {
            error_at(e->loc, "msgsend: requiere 2 argumentos (pid, valor)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_pid = lower_expr(e->args[0].get());
        const ir::IrValueId v_val = lower_expr(e->args[1].get());
        if (v_pid == ir::IR_NO_VALUE || v_val == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // AOT (native_poo_): mailbox por valor -> CALL __vx_msgsend(pid, val)
        // (sin buffer ni VM memory).  Devuelve 1 (ok) en el scheduler coop.
        if (native_poo_) {
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I32);
            ir::IrInstr ms{};
            ms.op = ir::IrOp::CALL;
            ms.func_name = "__vx_msgsend";
            ms.type = ir::IrType::I32;
            ms.dst = v_dst;
            ms.operands = {v_pid, v_val};
            ms.is_call_site = true;
            ms.source_line = e->loc.line;
            emit(current_block_, std::move(ms));
            out_value = v_dst;
            return true;
        }
        // ALLOCA 8 bytes en stack para el buffer del mensaje.
        const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr al{};
            al.op = ir::IrOp::ALLOCA;
            al.type = ir::IrType::I8;
            al.dst = v_buf;
            al.imm = 8;
            al.source_line = e->loc.line;
            emit(current_block_, std::move(al));
        }
        // STORE i64 v_val en v_buf.
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.operands = {v_val, v_buf};
            st.source_line = e->loc.line;
            emit(current_block_, std::move(st));
        }
        // msgsend r_pid, r_addr, r_len  -- usar IR op MSGSEND (0xD0) en lugar
        // de RAW_ASM.  Devuelve bool en R0 (1=ok, 0=error).
        const ir::IrValueId v_len = emit_const(ir::IrType::I64, 8, e->loc.line);
        const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I32);
        ir::IrInstr ms{};
        ms.op = ir::IrOp::MSGSEND;
        ms.type = ir::IrType::I32;
        ms.dst = v_dst;
        ms.operands = {v_pid, v_buf, v_len};
        ms.is_call_site = true;
        ms.source_line = e->loc.line;
        emit(current_block_, std::move(ms));
        out_value = v_dst;
        return true;
    }

    // ----- msgrecv() -----
    // Reservamos 8 bytes en el frame, llamamos msgrecv (bloquea si
    // mailbox vacio; al despertar el proceso re-ejecuta msgrecv y
    // pasa con datos), y leemos el i64 del buffer.
    if (is_msgrecv) {
        if (!e->args.empty()) {
            error_at(e->loc, "msgrecv: no acepta argumentos");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // AOT (native_poo_): mailbox por valor -> CALL __vx_msgrecv() -> i64
        // (lee de la mailbox de la tarea actual; no bloquea en Fase 2 -- el
        // mensaje ya esta porque main hace el setup antes de bombear).
        if (native_poo_) {
            const ir::IrValueId v_val = fn_->new_value(ir::IrType::I64);
            ir::IrInstr mr{};
            mr.op = ir::IrOp::CALL;
            mr.func_name = "__vx_msgrecv";
            mr.type = ir::IrType::I64;
            mr.dst = v_val;
            mr.is_call_site = true;
            mr.source_line = e->loc.line;
            emit(current_block_, std::move(mr));
            out_value = v_val;
            return true;
        }
        // ALLOCA 8 bytes para el buffer destino.
        const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr al{};
            al.op = ir::IrOp::ALLOCA;
            al.type = ir::IrType::I8;
            al.dst = v_buf;
            al.imm = 8;
            al.source_line = e->loc.line;
            emit(current_block_, std::move(al));
        }
        // msgrecv r_buf, r_max  -- primer reg = buffer dest, segundo = max len.
        // Convencion del decoder/exec: reg1=r_buf, reg2=r_max (ver
        // exec_instr_msgrecv en exec_instruction_distrib.cpp).
        const ir::IrValueId v_max = emit_const(ir::IrType::I64, 8, e->loc.line);
        {
            ir::IrInstr mr{};
            mr.op = ir::IrOp::MSGRECV;
            mr.type = ir::IrType::VOID;
            mr.dst = ir::IR_NO_VALUE;
            mr.operands = {v_buf, v_max};
            mr.is_call_site = true; // bloquea -> save/restore live regs
            mr.source_line = e->loc.line;
            emit(current_block_, std::move(mr));
        }
        // LOAD i64 desde v_buf -> v_val (resultado).
        const ir::IrValueId v_val = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_val;
            ld.operands = {v_buf};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
        }
        out_value = v_val;
        return true;
    }

    // ----- future_alloc() -----
    // Emite la instruccion bytecode `future` (0x29) que crea un nuevo
    // FutureObject GC-managed en estado PENDING y deposita su GcHandle
    // en R0.  Capturamos R0 a {dst} como i64 para pasarlo a fulfill/await.
    if (is_future_alloc) {
        if (!e->args.empty()) {
            error_at(e->loc, "future_alloc: no acepta argumentos");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // future -> aloca FutureObject; R0 contiene el handle.
        const ir::IrValueId v_fut = fn_->new_value(ir::IrType::I64);
        // AOT (native_poo_): CALL nativo al scheduler cooperativo
        // (__vx_future_new -> handle), bundle-ado desde vx_async.vx.
        ir::IrInstr fu{};
        fu.op = native_poo_ ? ir::IrOp::CALL : ir::IrOp::FUTURE;
        if (native_poo_) fu.func_name = "__vx_future_new";
        fu.type = ir::IrType::I64;
        fu.dst = v_fut;
        fu.is_call_site = true; // GC alloc
        fu.source_line = e->loc.line;
        emit(current_block_, std::move(fu));
        out_value = v_fut;
        return true;
    }

    // ----- fulfill(fut, value) -----
    // Emite `fulfill r_fut, r_val` (0x2B): resuelve el future con el
    // valor, despierta al waiter (si lo hay) via make_ready.  Devuelve
    // void (no captura R0).
    if (is_fulfill) {
        if (e->args.size() != 2) {
            error_at(e->loc, "fulfill: requiere 2 argumentos (fut, valor)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_fut = lower_expr(e->args[0].get());
        const ir::IrValueId v_val = lower_expr(e->args[1].get());
        if (v_fut == ir::IR_NO_VALUE || v_val == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // AOT (native_poo_): CALL nativo __vx_fulfill(fut, val).
        ir::IrInstr fu{};
        fu.op = native_poo_ ? ir::IrOp::CALL : ir::IrOp::FULFILL;
        if (native_poo_) fu.func_name = "__vx_fulfill";
        fu.type = ir::IrType::VOID;
        fu.dst = ir::IR_NO_VALUE;
        fu.operands = {v_fut, v_val};
        fu.source_line = e->loc.line;
        emit(current_block_, std::move(fu));
        out_value = ir::IR_NO_VALUE;
        return true;
    }

    return false;
}

} // namespace vx
