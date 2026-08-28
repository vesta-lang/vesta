/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/builtins_runtime.cpp
 * @brief Builtins que piden algo al MUNDO: ficheros, memoria del anfitrion,
 *        fibras y modulos cargados en marcha.
 *
 * Lo que tienen en comun es que ninguno se resuelve dentro del programa: hay
 * que pedirselo a alguien de fuera y quedarse con lo que devuelva -- un
 * descriptor, una direccion, un modulo --.  De ahi que casi todos compartan la
 * misma forma: preparar los argumentos, llamar a la nativa que corresponde, y
 * recoger el resultado sabiendo que puede fallar.
 *
 * Y de ahi tambien la parte que no es obvia: lo que se recibe es memoria del
 * ANFITRION, no de la maquina virtual, y hay que marcarla como tal en el valor
 * -- si no, quien lo lea despues emitiria un acceso a la memoria equivocada --.
 * Ese bit es la diferencia entre leer lo que se pidio y leer basura.
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
 * @brief Intenta bajar @p e como uno de los builtins que piden algo al mundo.
 * @return @c true si @p name era de esta familia y quedo bajado.
 */
bool Lowering::try_lower_runtime_builtins(ast::CallExpr *e,
                                          const std::string &name,
                                          ir::IrValueId &out_value) {
    const bool is_fopen = (name == "fopen");
    const bool is_fwrite = (name == "fwrite");
    const bool is_fclose = (name == "fclose");
    const bool is_malloc = (name == "malloc");
    const bool is_free = (name == "free");
    const bool is_fiber_swapctx = (name == "fiber_swapctx");
    const bool is_loadmodule = (name == "loadmodule");
    const bool is_unloadmodule = (name == "unloadmodule");
    const bool is_dispose = (name == "dispose");

    if (!(is_fopen || is_fwrite || is_fclose || is_malloc || is_free ||
          is_fiber_swapctx || is_loadmodule || is_unloadmodule || is_dispose))
        return false;

    // y emitir un STR_LIT_ADDR + CONST(len) en el bloque actual.
    // Devuelve par (str_ptr_ir_value, len_ir_value).

    // ----- fopen(path, mode) -> i64 -----
    // Args: (proc_ptr, path_addr, path_len, mode_addr, mode_len) = 5 args.
    // Devuelve uint64_t (FILE*).  Ambos args deben ser literales de string.
    if (is_fopen) {
        if (e->args.size() != 2 || !e->args[0] ||
            e->args[0]->kind != ast::NodeKind::StringLitExpr || !e->args[1] ||
            e->args[1]->kind != ast::NodeKind::StringLitExpr) {
            error_at(e->loc, "'fopen' requiere dos argumentos literales de "
                             "string (path, mode)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        auto *path = static_cast<ast::StringLitExpr *>(e->args[0].get());
        auto *mode = static_cast<ast::StringLitExpr *>(e->args[1].get());
        out_mod_->register_native_import(kVestaIoLib, "vio_fopen");

        const ir::IrValueId v_proc = emit_getproc(e->loc.line);
        auto [v_path, v_path_len] = emit_string_lit(path);
        auto [v_mode, v_mode_len] = emit_string_lit(mode);

        const ir::IrValueId dst = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLN;
        ins.type = ir::IrType::I64;
        ins.dst = dst;
        ins.func_name = kVestaIoLib + ":vio_fopen";
        ins.operands = {v_proc, v_path, v_path_len, v_mode, v_mode_len};
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        out_value = dst;
        return true;
    }

    // ----- fwrite(fp, buf) -> i64 -----
    // En Vesta la firma natural es fwrite(fp, buf), pero la firma C
    // de vesta_io es vio_fwrite(proc_ptr, vm_addr, size, handle).
    // El lowering reordena: (proc, buf_addr, buf_len, fp).
    if (is_fwrite) {
        if (e->args.size() != 2 || !e->args[1] ||
            e->args[1]->kind != ast::NodeKind::StringLitExpr) {
            error_at(e->loc, "'fwrite' requiere (FILE*, literal_string) - el "
                             "buffer debe ser literal");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        ir::IrValueId v_fp = lower_expr(e->args[0].get());
        if (v_fp == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        v_fp = cast_if_needed(v_fp, fn_->values[v_fp].type, ir::IrType::I64,
                              e->loc.line);
        auto *buf = static_cast<ast::StringLitExpr *>(e->args[1].get());
        out_mod_->register_native_import(kVestaIoLib, "vio_fwrite");

        const ir::IrValueId v_proc = emit_getproc(e->loc.line);
        auto [v_buf, v_buf_len] = emit_string_lit(buf);

        const ir::IrValueId dst = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLN;
        ins.type = ir::IrType::I64;
        ins.dst = dst;
        ins.func_name = kVestaIoLib + ":vio_fwrite";
        // Orden de args segun signature C: (proc, vm_addr, size, handle).
        ins.operands = {v_proc, v_buf, v_buf_len, v_fp};
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        out_value = dst;
        return true;
    }

    // ----- fclose(fp) -> i32 -----
    if (is_fclose) {
        if (e->args.size() != 1) {
            error_at(e->loc, "'fclose' requiere un argumento (FILE*)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        ir::IrValueId v_fp = lower_expr(e->args[0].get());
        if (v_fp == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        v_fp = cast_if_needed(v_fp, fn_->values[v_fp].type, ir::IrType::I64,
                              e->loc.line);
        out_mod_->register_native_import(kVestaIoLib, "vio_fclose");

        const ir::IrValueId dst = fn_->new_value(ir::IrType::I32);
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLN;
        ins.type = ir::IrType::I32;
        ins.dst = dst;
        ins.func_name = kVestaIoLib + ":vio_fclose";
        ins.operands = {v_fp};
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        out_value = dst;
        return true;
    }

    // ----- malloc(size) -----
    // Reserva un bloque host de `size` bytes y devuelve un void* con
    // is_host_ptr=true (LOAD/STORE consultan el flag para emitir movh).
    if (is_malloc) {
        if (e->args.size() != 1) {
            error_at(e->loc,
                     "'malloc' requiere exactamente un argumento de tamano");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        ir::IrValueId v_size = lower_expr(e->args[0].get());
        if (v_size == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        v_size = cast_if_needed(v_size, fn_->values[v_size].type,
                                ir::IrType::I64, e->loc.line);
        const ir::IrValueId dst = fn_->new_value(ir::IrType::PTR);
        // Marcar el resultado como puntero a memoria host: cualquier
        // LOAD/STORE posterior cuyo puntero descienda de este value
        // emitira movh en el ir_emitter.
        fn_->values[dst].is_host_ptr = true;
        ir::IrInstr ins{};
        ins.op = ir::IrOp::RAW_ALLOC;
        ins.type = ir::IrType::PTR;
        ins.dst = dst;
        ins.operands = {v_size};
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        out_value = dst;
        return true;
    }

    // ----- free(ptr) -----
    if (is_free) {
        if (e->args.size() != 1) {
            error_at(e->loc, "'free' requiere exactamente un puntero");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
        if (v_ptr == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
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

    // ----- fiber_swapctx(from_ctx, to_ctx) -----
    // Materializacion del primitivo de fiber-switch en el path INTERPRETE
    // (FN.1): emite el opcode VM `swapctx dst, src` (guarda el contexto actual
    // en
    // @p from_ctx y carga el de @p to_ctx).  El opcode guarda/restaura
    // {PC,SP,BP,R0..R15} (152 bytes) en memoria VM -> el cuerpo de fibra corre
    // como bytecode NORMAL con su estado VM, sin necesidad de @Naked ni asm
    // host.
    //
    //   exec_instr_swapctx: reg1 = dst (a CARGAR), reg2 = src (a GUARDAR).
    //   Semantica de fiber_swapctx(from, to): GUARDA en from, CARGA to.
    //   -> dst(load) = to_ctx = args[1] ; src(save) = from_ctx = args[0].
    //   IrOp::SWAPCTX espera operands[0]=dst_ctx, operands[1]=src_ctx.
    if (is_fiber_swapctx) {
        if (e->args.size() != 2) {
            error_at(e->loc,
                     "'fiber_swapctx' requiere dos direcciones de contexto VM "
                     "(from_ctx, to_ctx)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_from = lower_expr(e->args[0].get()); // src (save)
        const ir::IrValueId v_to = lower_expr(e->args[1].get());   // dst (load)
        if (v_from == ir::IR_NO_VALUE || v_to == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // El primitivo de fiber-switch baja por BACKEND:
        //   - INTERPRETE (.velb): opcode VM `swapctx` (estado VM, portable).
        //   - AOT nativo (native_poo_, FN.2): CALL al context-switch NATIVO
        //     `__vx_swapctx` (host-stack, @Naked de vx_fiber.vx; auto-bundle
        //     al detectarse la llamada).  Mismo layout de contexto {PC,SP,BP,
        //     callee-saved} que inicializa el llamante; el cuerpo de fibra ya
        //     es codigo nativo (native_poo), asi que el switch nativo funciona
        //     con el sin necesitar @Naked en el cuerpo.  Args en el mismo orden
        //     que los operandos de SWAPCTX: (to_ctx=cargar, from_ctx=guardar).
        if (native_poo_) {
            ir::IrInstr call{};
            call.op = ir::IrOp::CALL;
            call.func_name = "__vx_swapctx";
            call.type = ir::IrType::VOID;
            call.dst = ir::IR_NO_VALUE;
            call.is_call_site = true;
            call.operands = {v_to, v_from}; // arg0=to_ctx, arg1=from_ctx
            call.source_line = e->loc.line;
            emit(current_block_, std::move(call));
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        ir::IrInstr ins{};
        ins.op = ir::IrOp::SWAPCTX;
        ins.type = ir::IrType::VOID;
        ins.dst = ir::IR_NO_VALUE;
        ins.operands = {v_to,
                        v_from}; // operands[0]=dst_ctx, operands[1]=src_ctx
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        out_value = ir::IR_NO_VALUE;
        return true;
    }

    // ----- loadmodule(string_lit) -----
    // Carga dinamica de un .velb adicional.  Acepta solo string literal
    // (path al archivo en el filesystem del host).  Genera RAW_ASM que:
    //   1. Carga la direccion del path (interned en static_data) en un reg.
    //   2. Carga la longitud en bytes en otro reg.
    //   3. Emite `loadmod r_path, r_len`.  El opcode loadmod abre el file,
    //      llama a Loader::load_module_dynamic + automaticamente hace el
    //      callvm-equivalente al init_pc del modulo cargado (cuyo prologo
    //      registra clases via __module_init).  Cuando el main del modulo
    //      hace RET, el flujo continua aqui con R0 = init_pc (success) o
    //      0 (failure file not found / parse error).
    //   4. Captura R0 al SSA value.
    if (is_loadmodule) {
        if (e->args.size() != 1 || !e->args[0] ||
            e->args[0]->kind != ast::NodeKind::StringLitExpr) {
            error_at(
                e->loc,
                "loadmodule: requiere un string literal con la ruta al .velb");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        auto *slit = static_cast<ast::StringLitExpr *>(e->args[0].get());
        const uint64_t path_idx = intern_class_name(*out_mod_, slit->value);
        const uint32_t path_len = static_cast<uint32_t>(slit->value.size());
        // raw_asm-elim wave 2: usar STR_LIT_ADDR + emit_const + MOD_LOAD.
        const ir::IrValueId v_path_addr = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr sl{};
            sl.op = ir::IrOp::STR_LIT_ADDR;
            sl.type = ir::IrType::PTR;
            sl.dst = v_path_addr;
            sl.imm = path_idx;
            sl.source_line = e->loc.line;
            emit(current_block_, std::move(sl));
        }
        const ir::IrValueId v_path_len =
            emit_const(ir::IrType::I64, path_len, e->loc.line);
        const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ml{};
        ml.op = ir::IrOp::MOD_LOAD;
        ml.type = ir::IrType::I64;
        ml.dst = v_dst;
        ml.operands = {v_path_addr, v_path_len};
        ml.imm = 0; /* loadmod */
        ml.source_line = e->loc.line;
        ml.is_call_site = true;
        emit(current_block_, std::move(ml));
        out_value = v_dst;
        return true;
    }

    // ----- unloadmodule(string_lit) -> i32 -----
    // Descarga modulo dinamico previamente cargado.  Mismo patron que
    // loadmodule: path interned en static_data, opcode unloadmod r_addr, r_len.
    // Devuelve 1 si descargado, 0 si no encontrado.
    if (is_unloadmodule) {
        if (e->args.size() != 1 || !e->args[0] ||
            e->args[0]->kind != ast::NodeKind::StringLitExpr) {
            error_at(e->loc, "unloadmodule: requiere un string literal con la "
                             "ruta al .velb");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        auto *slit = static_cast<ast::StringLitExpr *>(e->args[0].get());
        const uint64_t path_idx = intern_class_name(*out_mod_, slit->value);
        const uint32_t path_len = static_cast<uint32_t>(slit->value.size());
        // raw_asm-elim wave 2: MOD_LOAD con kind=1 (unloadmod).
        const ir::IrValueId v_path_addr = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr sl{};
            sl.op = ir::IrOp::STR_LIT_ADDR;
            sl.type = ir::IrType::PTR;
            sl.dst = v_path_addr;
            sl.imm = path_idx;
            sl.source_line = e->loc.line;
            emit(current_block_, std::move(sl));
        }
        const ir::IrValueId v_path_len =
            emit_const(ir::IrType::I64, path_len, e->loc.line);
        const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I32);
        ir::IrInstr ml{};
        ml.op = ir::IrOp::MOD_LOAD;
        ml.type = ir::IrType::I32;
        ml.dst = v_dst;
        ml.operands = {v_path_addr, v_path_len};
        ml.imm = 1; /* unloadmod */
        ml.source_line = e->loc.line;
        emit(current_block_, std::move(ml));
        out_value = v_dst;
        return true;
    }

    // ===== Builtin dispose(xs) =====
    // Libera explicitamente una coleccion antes del exit del scope.
    // Emite CALLN al free fn correspondiente al tipo del local + reescribe
    // el binding local a 0 para que el cleanup automatico al exit pase
    // 0 al free fn (que es no-op por null-check interno).  Asi se evita
    // double-free.
    if (is_dispose) {
        if (e->args.size() != 1 || !e->args[0] ||
            e->args[0]->kind != ast::NodeKind::IdentExpr) {
            error_at(e->loc,
                     "dispose: requiere un IdentExpr local de tipo coleccion");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        auto *id_arg = static_cast<ast::IdentExpr *>(e->args[0].get());
        const Type arg_t = id_arg->result_type;
        if (!is_col_kind(arg_t.kind)) {
            error_at(e->loc, "dispose: el argumento no es de tipo coleccion");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ColType *ct = find_col_type(arg_t.kind);
        if (!ct) {
            error_at(e->loc, "dispose: tipo coleccion sin entry en COL_TYPES");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // 1. Lower del IdentExpr para obtener el handle actual.
        const ir::IrValueId v_handle = lower_expr(id_arg);
        if (v_handle == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // elegir variante *_free_gc cuando el local fue
        // declarado con tipo de elemento GC (ArrayList<string> etc.).
        PrimitiveKind elem_k = PrimitiveKind::VOID;
        PrimitiveKind val_k = PrimitiveKind::VOID;
        if (arg_t.pointee) elem_k = arg_t.pointee->kind;
        if (arg_t.pointee2) val_k = arg_t.pointee2->kind;
        const bool gc_aware = (ct->native_free_fn_gc != nullptr) &&
                              col_needs_gc_aware(arg_t.kind, elem_k, val_k);
        const char *fn_name =
            gc_aware ? ct->native_free_fn_gc : ct->native_free_fn;
        // 2. CALLN al free fn (idempotente por null-check del plugin).
        out_mod_->register_native_import(COL_NATIVE_LIB, fn_name);
        std::vector<ir::IrValueId> args;
        if (gc_aware) {
            args.reserve(2);
            args.push_back(emit_getproc(e->loc.line));
        } else {
            args.reserve(1);
        }
        args.push_back(v_handle);
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLN;
        ins.type = ir::IrType::VOID;
        ins.dst = ir::IR_NO_VALUE;
        ins.func_name = std::string(COL_NATIVE_LIB) + ":" + fn_name;
        ins.operands = std::move(args);
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        // 3. Reescribir el binding local a 0 (handle invalido).  El
        // cleanup al exit del scope vera este 0 (via refresh_name) y
        // sera no-op.  Evita double-free.
        const ir::IrValueId v_zero =
            emit_const(ir::IrType::I64, 0, e->loc.line);
        write_local(id_arg->name, v_zero, ir::IrType::I64, e->loc.line);
        out_value = ir::IR_NO_VALUE;
        return true;
    }

    return false;
}

} // namespace vx
