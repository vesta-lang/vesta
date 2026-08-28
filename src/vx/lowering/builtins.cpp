/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/builtins.cpp
 * @brief Bajada de las funciones que el lenguaje trae puestas.
 *
 * Los builtins de Vesta son las llamadas que el compilador reconoce por su
 * NOMBRE y baja el mismo, sin que exista una funcion que llamar: imprimir,
 * prestamos, atomicos, cadenas, reflexion, introspeccion en tiempo de
 * compilacion, colecciones, primitivas de concurrencia.
 *
 * Estaban dentro de lowering.cpp, en una funcion de mas de siete mil lineas --
 * el diecisiete por ciento de un fichero de cuarenta y tres mil --, y de ahi
 * sale: es un area con tema propio y se lee entera sin tener delante el resto
 * del lowering.
 *
 * Lo que queda por hacer, y conviene tenerlo escrito: dentro de esta funcion el
 * despacho sigue siendo una cascada de comparaciones -- ciento treinta
 * comparaciones de cadena que se evaluan SIEMPRE, y despues hasta ciento once
 * `if` encadenados, para bajar cualquier builtin --.  Repartir por familias y
 * despachar por tabla es el siguiente paso; este solo mueve el bloque de sitio,
 * sin tocar una linea de su logica.
 */
#include "util/env_flags.h"
#include "vx/lowering.h"
#include "vx/builtin_names.h" // reconocer el nombre de una vez, no comparandolo 200 veces
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

bool Lowering::try_lower_builtin_call(ast::CallExpr *e,
                                      ir::IrValueId &out_value) {
    // Solo manejamos identifier-callees (validado en lower_call).
    if (!e->callee || e->callee->kind != ast::NodeKind::IdentExpr) return false;
    const auto *id = static_cast<const ast::IdentExpr *>(e->callee.get());
    const std::string &name = id->name;

    /* Lo primero: ¿es siquiera un builtin?  Casi ninguna llamada de un
     * programa lo es -- son funciones del usuario --, y averiguarlo costaba
     * recorrer las familias y evaluar doscientas comparaciones de cadena antes
     * de contestar que no.  Una busqueda binaria lo decide de una vez, y ni
     * siquiera llega a buscar si la longitud del nombre no cae en el rango de
     * los que hay.
     *
     * Con UNA excepcion, y no es un detalle: un concepto usado como predicado
     * -- `Comparable<T>()` -- se baja aqui abajo, y su nombre lo pone el
     * usuario, asi que no puede estar en ninguna lista.  Lo que lo distingue
     * no es como se llama sino su FORMA: lleva argumentos de tipo y ninguno de
     * ejecucion.  Los conceptos que trae el lenguaje se invocan igual, asi que
     * la misma comprobacion los cubre a los dos. */
    const bool looks_like_concept = !e->type_args.empty() && e->args.empty();
    const Builtin b = builtin_from_name(name);
    if (!looks_like_concept && b == Builtin::Unknown) return false;

    /* Imprimir vive aparte (lowering/builtins_print.cpp): eran mil lineas de
     * las siete mil de esta funcion, y su trabajo -- averiguar QUE se escribe y
     * con que forma -- no se parece al del resto.  Contesta que no si el nombre
     * no es suyo, y aqui se sigue como siempre. */
    if (try_lower_print_builtins(e, b, out_value)) return true;
    /* Y lo que pide algo al MUNDO -- ficheros, memoria del anfitrion,
     * fibras, modulos cargados en marcha -- tampoco se parece al resto:
     * ninguno se resuelve dentro del programa. */
    if (try_lower_runtime_builtins(e, b, out_value)) return true;
    /* Y lo que supone que hay ALGUIEN MAS: memoria compartida, atomicos,
     * buzones y futuros.  Todos existen porque lo que uno escribe lo tiene
     * que ver el otro, y en el orden correcto. */
    if (try_lower_concurrent_builtins(e, b, out_value)) return true;
    /* Y lo que puede NO estar: Optional y Result.  Construirlos, preguntar si
     * hay algo, y sacarlo.  Van juntos porque los dos son la misma idea -- un
     * valor que lleva consigo si esta -- y porque ninguno toca el monton. */
    if (try_lower_optional_builtins(e, b, out_value)) return true;
    /* Y preguntarle al programa por si mismo: buscar la clase por su
     * nombre, el campo por el suyo, llamar a un metodo sin saber cual era
     * hasta ese momento.  Es lo contrario de todo lo demas, que se decide
     * al compilar. */
    if (try_lower_reflect_builtins(e, b, out_value)) return true;

    // -----------------------------------------------------------------
    // AOT 2c (dev OS): simbolos de seccion.  section_start/end(".x") -> void*,
    // section_size(".x") -> u64.  El arg debe ser un string LITERAL (el nombre
    // de la seccion); emitimos SECTION_REF(func_name=nombre, imm=kind).  El
    // writer AOT lo resuelve via reloc tras el layout; en interp/JIT -> 0.
    // -----------------------------------------------------------------
    if ((name == "section_start" || name == "section_end" ||
         name == "section_size") &&
        e->args.size() == 1) {
        auto *lit = dynamic_cast<ast::StringLitExpr *>(e->args[0].get());
        if (lit == nullptr || !lit->interp_parts.empty()) {
            error_at(e->loc, name + ": el argumento debe ser un string "
                                    "literal con el nombre de la seccion (e.g. "
                                    "\".boot\")");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const uint64_t kind = (name == "section_start") ? 0u
                              : (name == "section_end") ? 1u
                                                        : 2u;
        const ir::IrType rt =
            (name == "section_size") ? ir::IrType::I64 : ir::IrType::PTR;
        const ir::IrValueId dst = fn_->new_value(rt);
        // section_start/end devuelven un host_ptr real (la VA de la seccion);
        // marcarlo asi para que un LOAD/STORE posterior use el path host.
        if (kind != 2) fn_->values[dst].is_host_ptr = true;
        ir::IrInstr is{};
        is.op = ir::IrOp::SECTION_REF;
        is.type = rt;
        is.dst = dst;
        is.func_name = lit->value; // nombre de la seccion
        is.imm = kind;
        is.source_line = e->loc.line;
        emit(current_block_, std::move(is));
        out_value = dst;
        return true;
    }

    // -----------------------------------------------------------------
    // Sprint 1: builtins comptime de introspection.
    // Disparan SOLO cuando hay type_args.size()>=1.  Devuelven UN
    // valor constante computado a partir del tipo resuelto:
    //   sizeof<T>()   -> u64
    //   alignof<T>()  -> u64
    //   typename<T>() -> string (StringObject)
    //   type_id<T>()  -> u32
    //   kind<T>()     -> i32 (ComptimeKind enum)
    // Cero overhead runtime: la salida es un solo IrOp::CONST (o
    // STRMAKE para strings).
    // -----------------------------------------------------------------
    // Sprint B.1: as_native_callback(fn) -> i64 (host_ptr al thunk).
    //
    // Lowering: emite CALLN a vesta_runtime:vx_get_native_thunk con:
    //   r1 = @Absolute("code.<fn_name>")  (PC virtual de la fn Vesta)
    //   r2 = argc (numero de parametros que la fn Vesta recibe)
    // El runtime genera (o reusa) un thunk x86-64 callable con cc C
    // nativa y devuelve el host_ptr.
    if (name == "as_native_callback" && e->args.size() == 1) {
        auto *fn_id = dynamic_cast<ast::IdentExpr *>(e->args[0].get());
        if (fn_id == nullptr) {
            error_at(
                e->loc,
                "as_native_callback: arg debe ser identificador de fn Vesta");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        /* Resolver la signature de la fn Vesta para conocer argc. */
        uint32_t argc = 0;
        const FunctionSig *fsig = tc_.function_sig_by_name(fn_id->name);
        if (fsig != nullptr) {
            argc = (uint32_t)fsig->param_types.size();
        }
        const uint32_t src_line = e->loc.line;
        /* v_fn_pc = LABEL_ADDR("code.<fn_name>") */
        ir::IrValueId v_fn_pc = emit_label_addr(fn_id->name, src_line);
        /* v_argc = CONST i64 */
        ir::IrValueId v_argc =
            emit_const(ir::IrType::I64, (uint64_t)argc, src_line);
        /* CALLN @Method("vesta_runtime:vx_get_native_thunk", v_fn_pc, v_argc).
         */
        out_mod_->register_native_import("vesta_runtime",
                                         "vx_get_native_thunk");
        ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
        ir::IrInstr cl{};
        cl.op = ir::IrOp::CALLN;
        cl.type = ir::IrType::I64;
        cl.dst = v_dst;
        cl.func_name = "vesta_runtime:vx_get_native_thunk";
        cl.operands = {v_fn_pc, v_argc};
        cl.source_line = src_line;
        emit(current_block_, std::move(cl));
        out_value = v_dst;
        return true;
    }

    // fiber_entry(fn) -> VA de bytecode VM de la funcion (PC de arranque de una
    // fibra en el path INTERPRETE, FN.1).  Emite LABEL_ADDR("code.<fn>") sin
    // pasar por el enrutado a naked_fnaddr del cast `(cfn) fn` -> el `swapctx`
    // fija este PC y el interprete ejecuta el cuerpo como bytecode NORMAL.
    if (name == "fiber_entry" && e->args.size() == 1) {
        auto *fn_id = dynamic_cast<ast::IdentExpr *>(e->args[0].get());
        if (fn_id == nullptr) {
            error_at(e->loc,
                     "fiber_entry: arg debe ser identificador de fn cuerpo");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        out_value = emit_label_addr(fn_id->name, e->loc.line);
        return true;
    }

    // Overlay: `extent(v)` -> span total en runtime del layout de la vista.
    // Baja a un CALL a `__ovl_extent_<S>(base)`.
    if (name == "extent" && e->type_args.empty() && e->args.size() == 1) {
        ast::Expr *arg = e->args[0].get();
        const Type vt = arg->result_type;
        if (vt.kind != PrimitiveKind::STRUCT) {
            error_at(e->loc, "extent(v): v debe ser una vista @overlay");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const auto &lays = tc_.struct_layouts();
        auto it = lays.find(vt.struct_name);
        if (it == lays.end() || !it->second.is_overlay) {
            error_at(e->loc, "extent(v): '" + vt.struct_name +
                                 "' no es una vista @overlay");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId base = lower_expr(arg);
        if (base == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const std::string rname = generate_overlay_extent(it->second);
        ir::IrValueId d = fn_->new_value(ir::IrType::U64);
        ir::IrInstr in{};
        in.op = ir::IrOp::CALL;
        in.func_name = rname;
        in.type = ir::IrType::U64;
        in.dst = d;
        in.operands = {base};
        in.is_call_site = true;
        in.source_line = e->loc.line;
        emit(current_block_, std::move(in));
        out_value = d;
        return true;
    }

    // F4: `parent<T>()` dentro de un resolver `@offset { }` = el puntero de la
    // vista RAIZ, enhebrado como 2o param `root` (bind "__ovl_root").  Devuelve
    // ese puntero tipado como la vista T (el CallExpr ya tiene result_type=T).
    if (name == "parent" && !e->type_args.empty() && e->args.empty()) {
        const ir::IrValueId rv = lookup("__ovl_root");
        if (rv == ir::IR_NO_VALUE) {
            error_at(e->loc,
                     "parent<T>() solo es valido dentro de un resolver "
                     "@offset { } (de un overlay accedido como sub-vista)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        out_value = rv;
        return true;
    }

    // Overlay: forma-VALOR de offsetof / in_bounds (runtime).  Sin type
    // args; el 1er arg es un acceso a campo/elemento de una vista @overlay
    // (el type checker ya lo valido y fijo result_type a U64/BOOL).
    //   offsetof(v.campo)       = (u64) &campo - base_de_la_vista
    //   in_bounds(v.campo, len) = offsetof(v.campo) + sizeof(campo) <= len
    // Reusa la resolucion de direccion de lower_field_addr/lower_index_addr
    // (que ya resuelve @offset(expr) dinamico + stride de arrays); la raiz
    // de la cadena de accesos ES el puntero base construido con `T(buf)`.
    if (e->type_args.empty() && !e->args.empty() &&
        (name == "offsetof" || name == "in_bounds") &&
        (e->result_type.kind == PrimitiveKind::U64 ||
         e->result_type.kind == PrimitiveKind::BOOL)) {
        const uint32_t ln = e->loc.line;
        ast::Expr *arg = e->args[0].get();
        // Direccion (host) del campo/elemento accedido.
        ir::IrValueId field_addr = ir::IR_NO_VALUE;
        if (arg->kind == ast::NodeKind::FieldAccessExpr) {
            field_addr =
                lower_field_addr(static_cast<ast::FieldAccessExpr *>(arg));
        } else if (arg->kind == ast::NodeKind::IndexExpr) {
            field_addr = lower_index_addr(static_cast<ast::IndexExpr *>(arg));
        }
        if (field_addr == ir::IR_NO_VALUE) {
            error_at(e->loc, name + ": el argumento debe ser un acceso a "
                                    "campo/elemento de un overlay");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // Puntero base de la vista: raiz de la cadena de accesos.
        ast::Expr *root = arg;
        for (;;) {
            if (root->kind == ast::NodeKind::FieldAccessExpr) {
                root = static_cast<ast::FieldAccessExpr *>(root)->base.get();
                continue;
            }
            if (root->kind == ast::NodeKind::IndexExpr) {
                root = static_cast<ast::IndexExpr *>(root)->base.get();
                continue;
            }
            break;
        }
        const ir::IrValueId base_ptr = lower_expr(root);
        if (base_ptr == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // off = field_addr - base_ptr   (u64)
        ir::IrValueId off = fn_->new_value(ir::IrType::U64);
        {
            ir::IrInstr s{};
            s.op = ir::IrOp::SUB;
            s.type = ir::IrType::U64;
            s.dst = off;
            s.operands = {field_addr, base_ptr};
            s.source_line = ln;
            emit(current_block_, std::move(s));
        }
        if (name == "offsetof") {
            out_value = off;
            return true;
        }
        // in_bounds: (off + sizeof(campo)) <= len   (sin signo).
        const uint64_t fsize = comptime_type_size(tc_, arg->result_type);
        ir::IrValueId endv = off;
        if (fsize != 0) {
            ir::IrValueId fc = emit_const(ir::IrType::U64, fsize, ln);
            endv = fn_->new_value(ir::IrType::U64);
            ir::IrInstr a{};
            a.op = ir::IrOp::ADD;
            a.type = ir::IrType::U64;
            a.dst = endv;
            a.operands = {off, fc};
            a.source_line = ln;
            emit(current_block_, std::move(a));
        }
        ir::IrValueId lenv = lower_expr(e->args[1].get());
        if (lenv == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // Coaccionar len a u64 para una comparacion sin signo de 64 bits.
        const ir::IrType lfrom =
            ir_type_from_primitive(e->args[1]->result_type.kind);
        lenv = cast_if_needed(lenv, lfrom, ir::IrType::U64, ln);
        ir::IrValueId res = fn_->new_value(ir::IrType::BOOL);
        ir::IrInstr c{};
        c.op = ir::IrOp::CMP_ULE;
        c.type = ir::IrType::BOOL;
        c.dst = res;
        c.operands = {endv, lenv};
        c.source_line = ln;
        emit(current_block_, std::move(c));
        out_value = res;
        return true;
    }

    /* Type-metadata con arg LITERAL string: comptime_type_sizeof/alignof/kind
     * ("u64").  Son CONSTANTES compile-time -- se pliegan a un CONST para que
     * un @Macro que las use se baje a IR y corra por VM/JIT (no AST-eval).
     * El nombre del tipo se resuelve via resolve_type_string (misma ruta que
     * los typenames canonicos importados). */
    if (e->args.size() == 1 && e->args[0] &&
        e->args[0]->kind == ast::NodeKind::StringLitExpr &&
        (name == "comptime_type_sizeof" || name == "comptime_type_alignof" ||
         name == "comptime_type_kind")) {
        const std::string tn =
            static_cast<ast::StringLitExpr *>(e->args[0].get())->value;
        const Type t = tc_.resolve_type_string(tn);
        const uint32_t src_line = e->loc.line;
        if (name == "comptime_type_sizeof") {
            out_value = emit_const(ir::IrType::U64, comptime_type_size(tc_, t),
                                   src_line);
            return true;
        }
        if (name == "comptime_type_alignof") {
            out_value = emit_const(ir::IrType::U64, comptime_type_align(tc_, t),
                                   src_line);
            return true;
        }
        /* comptime_type_kind */
        const ComptimeKind k = comptime_type_kind(t);
        out_value = emit_const(ir::IrType::I32,
                               static_cast<uint64_t>(static_cast<int32_t>(k)),
                               src_line);
        return true;
    }

    // `bitcast<T>(v)`: reinterpretar los BITS.  El type checker ya exigio que
    // origen y destino midan lo mismo, asi que aqui es un BITCAST del IR -- que
    // entre tipos del mismo ancho baja a un `mov`: coste cero.
    if (name == "bitcast" && !e->type_args.empty() && e->args.size() == 1) {
        const Type dst_t = tc_.resolve_type_node(e->type_args[0].get());
        const ir::IrValueId v_src = lower_expr(e->args[0].get());
        if (v_src == ir::IR_NO_VALUE) return true;
        const ir::IrType dst_ir = ir_type_from_primitive(dst_t.kind);
        const ir::IrValueId v_dst = fn_->new_value(dst_ir);
        // Reinterpretar bits no cambia a que memoria apunta un puntero.
        fn_->values[v_dst].is_host_ptr = fn_->values[v_src].is_host_ptr;
        ir::IrInstr bc{};
        bc.op = ir::IrOp::BITCAST;
        bc.type = dst_ir;
        bc.dst = v_dst;
        bc.operands = {v_src};
        bc.source_line = e->loc.line;
        emit(current_block_, std::move(bc));
        out_value = v_dst;
        return true;
    }
    // Predicados de tipo: se responden en comptime -> una constante.  El `if`
    // que los use lo pliega el optimizer y la rama muerta desaparece: en el
    // binario solo queda el camino que corresponde a T.
    if (e->type_args.size() == 1 && e->args.empty() &&
        (name == "is_float" || name == "is_integer" || name == "is_signed" ||
         name == "is_unsigned" || name == "is_numeric" || name == "is_bool" ||
         name == "is_char" || name == "is_pointer" || name == "is_string" ||
         name == "is_class" || name == "is_struct" || name == "is_primitive" ||
         name == "is_enum")) {
        int64_t v = 0;
        if (!const_cast<TypeChecker &>(tc_).lsp_eval_builtin_scalar(e, &v)) {
            error_at(e->loc, "lowering: '" + name +
                                 "' no se pudo resolver en comptime");
            return true;
        }
        out_value =
            emit_const(ir::IrType::I8, (uint64_t)(v != 0 ? 1 : 0), e->loc.line);
        return true;
    }
    if (!e->type_args.empty() &&
        (name == "sizeof" || name == "alignof" || name == "typename" ||
         name == "type_id" || name == "kind")) {
        const Type t = tc_.resolve_type_node(e->type_args[0].get());
        const uint32_t src_line = e->loc.line;
        if (name == "sizeof") {
            const uint64_t v = comptime_type_size(tc_, t);
            out_value = emit_const(ir::IrType::U64, v, src_line);
            return true;
        }
        if (name == "alignof") {
            const uint64_t v = comptime_type_align(tc_, t);
            out_value = emit_const(ir::IrType::U64, v, src_line);
            return true;
        }
        if (name == "type_id") {
            const uint32_t v = comptime_type_id(tc_, t);
            out_value =
                emit_const(ir::IrType::U32, static_cast<uint64_t>(v), src_line);
            return true;
        }
        if (name == "kind") {
            const ComptimeKind k = comptime_type_kind(t);
            out_value = emit_const(
                ir::IrType::I32, static_cast<uint64_t>(static_cast<int32_t>(k)),
                src_line);
            return true;
        }
        /* typename<T>() -> StringObject construido inline desde el
         * nombre canonico.  Reusa el mismo patron que el path no-
         * interpolado de @c lower_string_literal_to_string_object:
         * STR_LIT_ADDR a static_data + RAW_ASM strmake. */
        if (name == "typename") {
            const std::string nm = comptime_type_name(tc_, t);
            std::vector<uint8_t> bytes(nm.begin(), nm.end());
            const uint64_t idx = out_mod_->intern_static_data(std::move(bytes));
            ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr ins{};
                ins.op = ir::IrOp::STR_LIT_ADDR;
                ins.type = ir::IrType::PTR;
                ins.dst = v_addr;
                ins.imm = idx;
                ins.source_line = src_line;
                emit(current_block_, std::move(ins));
            }
            ir::IrValueId v_len = emit_const(
                ir::IrType::I64, static_cast<uint64_t>(nm.size()), src_line);
            /* typename es un literal compile-time: value-string nativo en AOT
             * (PURE_NATIVE) o GcHandle en el resto de tiers. */
            ir::IrValueId v_str = emit_string_literal_repr(
                v_addr, v_len, (int64_t)nm.size(), src_line);
            out_value = v_str;
            return true;
        }
    }

    // -----------------------------------------------------------------
    // A.39: builtins comptime sobre strings.  Args ya validados como
    // comptime-evaluables por type_checker.  Aqui evaluamos y emitimos:
    //   comptime_concat -> STRMAKE inline con bytes concatenados
    //   comptime_streq  -> CONST bool con resultado
    //   comptime_strlen -> CONST u64 con size
    // -----------------------------------------------------------------
    if (name == "comptime_concat" || name == "comptime_streq" ||
        name == "comptime_strlen") {
        const ComptimeEvalResult r = comptime_eval_expr(tc_, e);
        if (!r.ok) {
            /*  MC.24: si el comptime eval falla (e.g. arg es
             * un comptime var que solo se resuelve en call-site
             * del macro padre), NO erroreamos.  En su lugar, dejamos
             * que el path runtime (str_concat/str_equals/str_length
             * via STRCAT/STRCMP/STRLEN bytecode) maneje el call.
             * El macro corre via VM al invocarse y produce el
             * resultado correcto.  Solo fallback si el caller es
             * NO un macro (en cuyo caso si hay error real). */
            /* Caer al lowering normal abajo via is_str_concat/etc. */
            /* Fall through. */
        } else {
            const uint32_t src_line = e->loc.line;
            if (r.is_str) {
                /* Materializar como StringObject inline. */
                std::vector<uint8_t> bytes(r.str.begin(), r.str.end());
                const uint64_t idx =
                    out_mod_->intern_static_data(std::move(bytes));
                ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
                {
                    ir::IrInstr is{};
                    is.op = ir::IrOp::STR_LIT_ADDR;
                    is.type = ir::IrType::PTR;
                    is.dst = v_addr;
                    is.imm = idx;
                    is.source_line = src_line;
                    emit(current_block_, std::move(is));
                }
                ir::IrValueId v_len = emit_const(
                    ir::IrType::I64, (uint64_t)r.str.size(), src_line);
                ir::IrValueId v_str =
                    emit_string_literal_repr(v_addr, v_len, -1, src_line);
                out_value = v_str;
                return true;
            }
            /* Int result: comptime_streq -> bool, comptime_strlen -> u64. */
            ir::IrType t =
                (name == "comptime_streq") ? ir::IrType::BOOL : ir::IrType::U64;
            out_value = emit_const(t, (uint64_t)r.value, src_line);
            return true;
        } /* end else block (r.ok=true path) */
    }

    // -----------------------------------------------------------------
    // static_assert(cond, "msg").
    //
    // Con la condicion comptime-constante la resuelve el type checker y aqui
    // no se emite nada, como siempre.  Cuando NO lo es -- porque depende de un
    // parametro de la propia comptime fn, que es el caso interesante -- el
    // type checker calla a proposito y cuenta con que se baje a
    // `vesta_comptime:static_assert`, para que la evalue la ComptimeVM al
    // ejecutar el cuerpo (que sigue siendo tiempo de compilacion).  Esa
    // segunda ruta ya estaba escrita mas abajo pero un descarte incondicional
    // se la comia, dejando a una comptime fn sin forma de rechazar su entrada.
    //
    // Fuera de un cuerpo comptime se sigue descartando: emitir la llamada solo
    // meteria trabajo en el programa final (la stdlib declara decenas de
    // guards por tipo generico, y cada uno se volvia un CALLN de verdad).
    // -----------------------------------------------------------------
    if (name == "static_assert" && !current_fn_is_macro_) {
        out_value = ir::IR_NO_VALUE;
        return true;
    }

    // -----------------------------------------------------------------
    // Sprint 4 (A.37.s4): builtins runtime de introspection.
    //   find_type("Lit")                   -> direct mov al chunk
    //   find_type(s)                       -> CALL al resolver runtime (TODO)
    //   type_info_kind/size/align/field_count -> LOAD u32 con offset fijo
    //   type_info_name(p) / _field_name(p, i) -> construye StringObject
    //   type_info_field_offset / _field_size  -> LOAD u32 con stride 16
    // -----------------------------------------------------------------
    {
        const bool is_find = (name == "find_type");
        const bool is_simple_u32 = name == "type_info_size" ||
                                   name == "type_info_align" ||
                                   name == "type_info_field_count";
        const bool is_kind_i32 = (name == "type_info_kind");
        const bool is_name_q = (name == "type_info_name");
        const bool is_field_name = (name == "type_info_field_name");
        const bool is_field_u32 =
            name == "type_info_field_offset" || name == "type_info_field_size";
        if (is_find || is_simple_u32 || is_kind_i32 || is_name_q ||
            is_field_name || is_field_u32) {
            const uint32_t src_line = e->loc.line;
            if (is_find) {
                /* Resolver literal -> chunk idx en compile-time.
                 * Caso runtime string deferido a Sprint 5. */
                auto *slit =
                    e->args.empty()
                        ? nullptr
                        : dynamic_cast<ast::StringLitExpr *>(e->args[0].get());
                if (slit && !slit->is_interpolated()) {
                    auto it = introspect_idx_by_name_.find(slit->value);
                    if (it == introspect_idx_by_name_.end()) {
                        /* Tipo no registrado con @Introspect -> 0. */
                        out_value = emit_const(ir::IrType::I64, 0, src_line);
                        return true;
                    }
                    // raw_asm-elim wave 2: usar IrOp::STR_LIT_ADDR
                    // que ya emite `mov {dst}, @Absolute("code.s_N")`.
                    ir::IrValueId dst = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr sl{};
                    sl.op = ir::IrOp::STR_LIT_ADDR;
                    sl.type = ir::IrType::PTR;
                    sl.dst = dst;
                    sl.imm = static_cast<uint64_t>(it->second);
                    sl.source_line = src_line;
                    emit(current_block_, std::move(sl));
                    out_value = dst;
                    return true;
                }
                /* Runtime string: para MVP devolvemos 0 (no soportado).
                 * Sprint 5 añade resolver sintetico. */
                error_at(e->loc,
                         "find_type: en MVP solo se soporta literal string "
                         "(runtime resolver pendiente en Sprint 5)");
                out_value = emit_const(ir::IrType::I64, 0, src_line);
                return true;
            }
            /* Resto: el primer arg es el handle del IntrospectInfo. */
            if (e->args.empty()) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId info_ptr = lower_expr(e->args[0].get());
            if (info_ptr == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            /* Helper: emite ADD ptr + offset_const + LOAD U32. */
            auto emit_load_u32_at = [&](uint32_t offset) -> ir::IrValueId {
                ir::IrValueId addr;
                if (offset == 0) {
                    addr = info_ptr;
                } else {
                    ir::IrValueId off_val =
                        emit_const(ir::IrType::I64, offset, src_line);
                    addr = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr ad{};
                    ad.op = ir::IrOp::ADD;
                    ad.type = ir::IrType::I64;
                    ad.dst = addr;
                    ad.operands = {info_ptr, off_val};
                    ad.source_line = src_line;
                    emit(current_block_, std::move(ad));
                }
                ir::IrValueId dst = fn_->new_value(ir::IrType::U32);
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD;
                ld.type = ir::IrType::U32;
                ld.dst = dst;
                ld.operands = {addr};
                ld.source_line = src_line;
                emit(current_block_, std::move(ld));
                return dst;
            };
            if (is_kind_i32) {
                /* kind vive en offset 0 como u32; el tipo de retorno
                 * declarado es i32 asi que el caller ve un i32 (mismos
                 * bits). */
                ir::IrValueId v = emit_load_u32_at(0);
                out_value = v;
                return true;
            }
            if (is_simple_u32) {
                uint32_t off = 0;
                if (name == "type_info_size")
                    off = 4;
                else if (name == "type_info_align")
                    off = 8;
                else if (name == "type_info_field_count")
                    off = 12;
                out_value = emit_load_u32_at(off);
                return true;
            }
            if (is_name_q) {
                /* type_info_name(p): name_off = LOAD u32 [p+16],
                 * name_len = LOAD u32 [p+20], addr = p + name_off,
                 * STRMAKE(addr, name_len). */
                ir::IrValueId name_off = emit_load_u32_at(16);
                ir::IrValueId name_len = emit_load_u32_at(20);
                /* Promote name_off a i64 antes del ADD. */
                ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = addr;
                ad.operands = {info_ptr, name_off};
                ad.source_line = src_line;
                emit(current_block_, std::move(ad));
                /* STRMAKE necesita addr y len.  Para name_len que es u32
                 * lo usamos como i64 directamente; en la VM ambos caben
                 * en qword. */
                ir::IrValueId v_str =
                    emit_string_literal_repr(addr, name_len, -1, src_line);
                out_value = v_str;
                return true;
            }
            /* type_info_field_*: segundo arg es idx (u32). */
            if (e->args.size() < 2) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId idx_val = lower_expr(e->args[1].get());
            /* field_addr = info_ptr + 24 + idx * 16 */
            ir::IrValueId v16 = emit_const(ir::IrType::I64, 16, src_line);
            ir::IrValueId idx_x16 = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr mu{};
                mu.op = ir::IrOp::MUL;
                mu.type = ir::IrType::I64;
                mu.dst = idx_x16;
                mu.operands = {idx_val, v16};
                mu.source_line = src_line;
                emit(current_block_, std::move(mu));
            }
            ir::IrValueId v24 = emit_const(ir::IrType::I64, 24, src_line);
            ir::IrValueId field_off = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = field_off;
                ad.operands = {idx_x16, v24};
                ad.source_line = src_line;
                emit(current_block_, std::move(ad));
            }
            ir::IrValueId field_addr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = field_addr;
                ad.operands = {info_ptr, field_off};
                ad.source_line = src_line;
                emit(current_block_, std::move(ad));
            }
            /* Helper interno LOAD u32 at field_addr + offset. */
            auto load_u32_field = [&](uint32_t off) -> ir::IrValueId {
                ir::IrValueId off_val =
                    emit_const(ir::IrType::I64, off, src_line);
                ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
                {
                    ir::IrInstr ad{};
                    ad.op = ir::IrOp::ADD;
                    ad.type = ir::IrType::I64;
                    ad.dst = addr;
                    ad.operands = {field_addr, off_val};
                    ad.source_line = src_line;
                    emit(current_block_, std::move(ad));
                }
                ir::IrValueId dst = fn_->new_value(ir::IrType::U32);
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD;
                ld.type = ir::IrType::U32;
                ld.dst = dst;
                ld.operands = {addr};
                ld.source_line = src_line;
                emit(current_block_, std::move(ld));
                return dst;
            };
            if (name == "type_info_field_offset") {
                out_value = load_u32_field(0);
                return true;
            }
            if (name == "type_info_field_size") {
                out_value = load_u32_field(4);
                return true;
            }
            if (is_field_name) {
                /* field_addr+8 = name_off; field_addr+12 = name_len */
                ir::IrValueId fname_off = load_u32_field(8);
                ir::IrValueId fname_len = load_u32_field(12);
                ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
                {
                    ir::IrInstr ad{};
                    ad.op = ir::IrOp::ADD;
                    ad.type = ir::IrType::I64;
                    ad.dst = addr;
                    ad.operands = {info_ptr, fname_off};
                    ad.source_line = src_line;
                    emit(current_block_, std::move(ad));
                }
                ir::IrValueId v_str =
                    emit_string_literal_repr(addr, fname_len, -1, src_line);
                out_value = v_str;
                return true;
            }
        }
    }

    // -----------------------------------------------------------------
    // Sprint 3-C introspection: for_each_field<T>(cb) / for_each_method.
    // Loop completamente unrolled en compile-time: por cada field/
    // method de T emitimos UNA invocacion CALLCLOSURE al callback
    // con el nombre como string.  Cero overhead de loop runtime
    // (vs map dinamico), pero N llamadas reales al callback.
    // -----------------------------------------------------------------
    if (!e->type_args.empty() &&
        (name == "for_each_field" || name == "for_each_method")) {
        const bool is_fields = (name == "for_each_field");
        const Type t = tc_.resolve_type_node(e->type_args[0].get());
        if (e->args.empty()) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        /* Lower el callback una sola vez -> fv_addr (16 bytes en stack). */
        const ir::IrValueId fv_addr = lower_expr(e->args[0].get());
        if (fv_addr == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        /* LOAD fn_addr = [fv_addr]; LOAD env_addr = [fv_addr + 8]. */
        ir::IrValueId fn_addr = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = fn_addr;
            ld.operands = {fv_addr};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
        }
        ir::IrValueId env_addr = fn_->new_value(ir::IrType::I64);
        {
            ir::IrValueId fv_plus_8 = fn_->new_value(ir::IrType::PTR);
            ir::IrValueId off8 = emit_const(ir::IrType::I64, 8, e->loc.line);
            {
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = fv_plus_8;
                ad.operands = {fv_addr, off8};
                ad.source_line = e->loc.line;
                emit(current_block_, std::move(ad));
            }
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = env_addr;
            ld.operands = {fv_plus_8};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
        }
        /* Iterar fields/methods y emitir una CALLCLOSURE por cada uno. */
        const uint32_t n = is_fields ? comptime_field_count(tc_, t)
                                     : comptime_method_count(tc_, t);
        for (uint32_t i = 0; i < n; ++i) {
            const std::string nm =
                is_fields
                    ? comptime_field_name(tc_, t, i)
                    : (i < comptime_method_count(tc_, t) ? [&]() {
                          /* Buscar el i-esimo method name. */
                          if (t.kind == PrimitiveKind::STRUCT ||
                              t.kind == PrimitiveKind::CLASS) {
                              auto it = tc_.class_layouts().find(t.struct_name);
                              if (it != tc_.class_layouts().end() &&
                                  i < it->second.methods.size()) {
                                  return it->second.methods[i].name;
                              }
                          }
                          return std::string();
                      }()
                                                         : std::string());
            if (nm.empty()) continue;
            /* Build StringObject para el nombre. */
            std::vector<uint8_t> bytes(nm.begin(), nm.end());
            const uint64_t idx = out_mod_->intern_static_data(std::move(bytes));
            ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr is{};
                is.op = ir::IrOp::STR_LIT_ADDR;
                is.type = ir::IrType::PTR;
                is.dst = v_addr;
                is.imm = idx;
                is.source_line = e->loc.line;
                emit(current_block_, std::move(is));
            }
            ir::IrValueId v_len = emit_const(
                ir::IrType::I64, static_cast<uint64_t>(nm.size()), e->loc.line);
            ir::IrValueId v_str =
                emit_string_literal_repr(v_addr, v_len, -1, e->loc.line);
            /* CALLCLOSURE(env_addr, v_str) -- void return. */
            ir::IrInstr cl{};
            cl.op = ir::IrOp::CALLCLOSURE;
            cl.type = ir::IrType::VOID;
            cl.dst = ir::IR_NO_VALUE;
            cl.func_ptr = fn_addr;
            cl.operands = {env_addr, v_str};
            cl.source_line = e->loc.line;
            emit(current_block_, std::move(cl));
        }
        out_value = ir::IR_NO_VALUE;
        return true;
    }

    // -----------------------------------------------------------------
    // Sprint 3-A introspection: field_get<T>(obj, "f") / field_set.
    // Bypass de getfield/setfield: usa offset compile-time via
    // comptime_field_offset.  El type checker ya valido tipos y
    // que el segundo arg sea string literal.
    // -----------------------------------------------------------------
    if (!e->type_args.empty() && (name == "field_get" || name == "field_set")) {
        const bool is_get = (name == "field_get");
        const Type t = tc_.resolve_type_node(e->type_args[0].get());
        std::string fname;
        if (e->args.size() >= 2) {
            if (auto *slit =
                    dynamic_cast<ast::StringLitExpr *>(e->args[1].get())) {
                fname = slit->value;
            }
        }
        const int64_t off = comptime_field_offset(tc_, t, fname);
        if (off < 0) {
            error_at(e->loc, name + ": el tipo '" + comptime_type_name(tc_, t) +
                                 "' no tiene campo '" + fname + "'");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const Type ftype = comptime_field_type(tc_, t, fname);
        const ir::IrType ir_t = ir_type_from_primitive(ftype.kind);
        /* Lower obj: el primer arg.  Para CLASS el SSA value es un
         * host_ptr al ObjectHeader; para STRUCT inline es la direccion
         * VM del slot (resultado de la ALLOCA o del campo padre).
         * Detectamos por el TIPO declarado en T (no por el resultado de
         * check_expr del arg, que podria ser COUNT/inferido). */
        const ir::IrValueId obj = lower_expr(e->args[0].get());
        if (obj == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        /* Para CLASS la convencion del codegen es is_host_ptr=true sobre
         * el resultado de NEWOBJ/__new_<X> (host_ptr al ObjectHeader).
         * Para STRUCT la convencion es is_host_ptr=false (slot VM).
         * NO usamos emit_field_addr aqui porque su shortcut offset==0
         * MUTA el flag is_host_ptr del base SSA value (rompe init-lists
         * anteriores que comparten el binding).  En su lugar emitimos
         * un ADD i64 explicito que produce un nuevo SSA value distinto
         * del base, y propagamos is_host_ptr/pointee_is_host_ptr segun
         * la naturaleza de T. */
        const bool t_is_class = (t.kind == PrimitiveKind::CLASS);
        ir::IrValueId addr;
        if (off == 0) {
            addr = obj;
        } else {
            ir::IrValueId off_val = fn_->new_value(ir::IrType::I64);
            fn_->values[off_val].is_const = true;
            fn_->values[off_val].const_val = static_cast<uint64_t>(off);
            {
                ir::IrInstr c{};
                c.op = ir::IrOp::CONST;
                c.type = ir::IrType::I64;
                c.dst = off_val;
                c.imm = static_cast<uint64_t>(off);
                c.source_line = e->loc.line;
                emit(current_block_, std::move(c));
            }
            addr = fn_->new_value(ir::IrType::PTR);
            fn_->values[addr].is_host_ptr = t_is_class;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = addr;
            ad.operands = {obj, off_val};
            ad.source_line = e->loc.line;
            emit(current_block_, std::move(ad));
        }
        /* Si offset==0 y T es CLASS, el obj YA debe tener is_host_ptr.
         * Si T es STRUCT con offset==0, el slot VM se mantiene sin
         * tocar el flag (heredamos el state del obj, que ya es lo
         * correcto). */
        if (is_get) {
            const ir::IrValueId dst = fn_->new_value(ir_t);
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir_t;
            ld.dst = dst;
            ld.operands = {addr};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
            /* Propagar is_host_ptr para campos PTR no virtuales (mismo
             * tratamiento que lower_class_field_load). */
            if (ftype.kind == PrimitiveKind::PTR && !ftype.is_virtual) {
                fn_->values[dst].is_host_ptr = true;
            }
            /* Campo CLASS: el slot guarda un GcHandle, no un host_ptr.
             * Hacemos gcderef para obtener host_ptr fresco post-GC. */
            if (ftype.kind == PrimitiveKind::CLASS) {
                // raw_asm-elim 2026-05-28: gcderef + xchg ->
                // IrOp::GC_DEREF_HOST.
                ir::IrValueId v_host = fn_->new_value(ir::IrType::I64);
                fn_->values[v_host].is_host_ptr = true;
                fn_->values[v_host].is_gc_object = true;
                ir::IrInstr deref{};
                deref.op = ir::IrOp::GC_DEREF_HOST;
                deref.type = ir::IrType::PTR;
                deref.dst = v_host;
                deref.operands = {dst};
                deref.source_line = e->loc.line;
                emit(current_block_, std::move(deref));
                out_value = v_host;
                return true;
            }
            out_value = dst;
            return true;
        }
        /* field_set: lower value y emit STORE. */
        if (e->args.size() < 3) {
            /* Type checker ya emitio error; salir limpio. */
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        ir::IrValueId val = lower_expr(e->args[2].get());
        if (val == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        /* Coerce el valor al tipo del campo si difiere.  El SSA value
         * de val ya tiene su tipo en fn_->values[val].type; el cast
         * inserta truncate/sext/zext segun signos y anchos. */
        const ir::IrType val_t = fn_->values[val].type;
        val = cast_if_needed(val, val_t, ir_t, e->loc.line);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir_t;
        st.dst = ir::IR_NO_VALUE;
        /* Convencion IR: operands = {value, addr} (no al reves). */
        st.operands = {val, addr};
        st.source_line = e->loc.line;
        emit(current_block_, std::move(st));
        out_value = ir::IR_NO_VALUE;
        return true;
    }

    // -----------------------------------------------------------------
    // Concepto como PREDICADO: `Concepto<T>()` -> CONST bool.  El type
    // checker ya valido que sea un concepto (built-in o de usuario) con
    // 1 type-arg y 0 args runtime.
    // -----------------------------------------------------------------
    if (!e->type_args.empty() && e->args.empty() &&
        (is_builtin_concept(name) ||
         tc_.concepts().find(name) != tc_.concepts().end())) {
        const uint32_t src_line = e->loc.line;
        const Type t1 = tc_.resolve_type_node(e->type_args[0].get());
        const ConceptEval ce = comptime_eval_concept(tc_, name, t1);
        out_value =
            emit_const(ir::IrType::BOOL,
                       (ce.found && ce.satisfied) ? 1ULL : 0ULL, src_line);
        return true;
    }

    // -----------------------------------------------------------------
    // Sprint 2 introspection: 12 builtins de fields/methods/types.
    // Mismas garantias que Sprint 1: cada llamada baja a UN solo
    // IrOp::CONST (o STR_LIT_ADDR + STRMAKE para los que devuelven
    // string).  El type checker ya valido aridad + que los args
    // runtime sean literales compile-time.
    // -----------------------------------------------------------------
    {
        const bool one_targ_no_args =
            name == "field_count" || name == "method_count" ||
            name == "is_class" || name == "is_struct" ||
            name == "is_primitive" || name == "is_enum" ||
            name == "is_newtype" || name == "is_opaque" ||
            name == "underlying_of";
        const bool one_targ_str_arg =
            name == "offsetof" || name == "has_field" || name == "has_method" ||
            name == "field_type";
        const bool one_targ_int_arg = (name == "field_name");
        const bool two_targ_no_args = name == "is_subtype" || name == "is_same";

        if ((one_targ_no_args || one_targ_str_arg || one_targ_int_arg ||
             two_targ_no_args) &&
            !e->type_args.empty()) {
            const uint32_t src_line = e->loc.line;
            const Type t1 = tc_.resolve_type_node(e->type_args[0].get());
            /* Helper local: emite STRMAKE con el nombre canonico recibido. */
            auto emit_strmake_for =
                [&](const std::string &nm) -> ir::IrValueId {
                std::vector<uint8_t> bytes(nm.begin(), nm.end());
                const uint64_t idx =
                    out_mod_->intern_static_data(std::move(bytes));
                ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
                {
                    ir::IrInstr is{};
                    is.op = ir::IrOp::STR_LIT_ADDR;
                    is.type = ir::IrType::PTR;
                    is.dst = v_addr;
                    is.imm = idx;
                    is.source_line = src_line;
                    emit(current_block_, std::move(is));
                }
                ir::IrValueId v_len =
                    emit_const(ir::IrType::I64,
                               static_cast<uint64_t>(nm.size()), src_line);
                ir::IrValueId v_str =
                    emit_string_literal_repr(v_addr, v_len, -1, src_line);
                return v_str;
            };

            /* Extraer el arg literal compile-time si lo hay. */
            std::string slit_arg;
            uint64_t ilit_arg = 0;
            if (one_targ_str_arg) {
                auto *slit =
                    dynamic_cast<ast::StringLitExpr *>(e->args[0].get());
                if (slit) slit_arg = slit->value;
            }
            if (one_targ_int_arg) {
                auto *ilit = dynamic_cast<ast::IntLitExpr *>(e->args[0].get());
                if (ilit) ilit_arg = ilit->value;
            }

            if (name == "field_count") {
                const uint32_t v = comptime_field_count(tc_, t1);
                out_value = emit_const(ir::IrType::U32, v, src_line);
                return true;
            }
            if (name == "method_count") {
                const uint32_t v = comptime_method_count(tc_, t1);
                out_value = emit_const(ir::IrType::U32, v, src_line);
                return true;
            }
            if (name == "is_class") {
                const bool v = comptime_is_class(t1);
                out_value =
                    emit_const(ir::IrType::BOOL, v ? 1ULL : 0ULL, src_line);
                return true;
            }
            if (name == "is_struct") {
                const bool v = comptime_is_struct(tc_, t1);
                out_value =
                    emit_const(ir::IrType::BOOL, v ? 1ULL : 0ULL, src_line);
                return true;
            }
            if (name == "is_enum") {
                const bool v = comptime_is_enum(tc_, t1);
                out_value =
                    emit_const(ir::IrType::BOOL, v ? 1ULL : 0ULL, src_line);
                return true;
            }
            if (name == "is_primitive") {
                const bool v = comptime_is_primitive(t1);
                out_value =
                    emit_const(ir::IrType::BOOL, v ? 1ULL : 0ULL, src_line);
                return true;
            }
            if (name == "is_newtype") {
                const bool v = comptime_is_newtype(t1);
                out_value =
                    emit_const(ir::IrType::BOOL, v ? 1ULL : 0ULL, src_line);
                return true;
            }
            if (name == "is_opaque") {
                const bool v = comptime_is_opaque(t1);
                out_value =
                    emit_const(ir::IrType::BOOL, v ? 1ULL : 0ULL, src_line);
                return true;
            }
            if (name == "underlying_of") {
                const std::string un = comptime_underlying_name(tc_, t1);
                out_value = emit_strmake_for(un);
                return true;
            }
            if (name == "offsetof") {
                const int64_t off = comptime_field_offset(tc_, t1, slit_arg);
                /* off==-1 (campo no existe): emitir error claro y
                 * usar 0 como fallback para no romper el flujo. */
                if (off < 0) {
                    diags_.error(e->loc, "offsetof: el tipo '" +
                                             comptime_type_name(tc_, t1) +
                                             "' no tiene campo '" + slit_arg +
                                             "'");
                }
                out_value = emit_const(
                    ir::IrType::U64,
                    off < 0 ? 0ULL : static_cast<uint64_t>(off), src_line);
                return true;
            }
            if (name == "has_field") {
                const bool v = comptime_has_field(tc_, t1, slit_arg);
                out_value =
                    emit_const(ir::IrType::BOOL, v ? 1ULL : 0ULL, src_line);
                return true;
            }
            if (name == "has_method") {
                const bool v = comptime_has_method(tc_, t1, slit_arg);
                out_value =
                    emit_const(ir::IrType::BOOL, v ? 1ULL : 0ULL, src_line);
                return true;
            }
            if (name == "field_type") {
                const std::string tn =
                    comptime_field_type_name(tc_, t1, slit_arg);
                if (tn.empty()) {
                    diags_.error(e->loc, "field_type: el tipo '" +
                                             comptime_type_name(tc_, t1) +
                                             "' no tiene campo '" + slit_arg +
                                             "'");
                }
                out_value = emit_strmake_for(tn);
                return true;
            }
            if (name == "field_name") {
                const std::string nm_v = comptime_field_name(
                    tc_, t1, static_cast<uint32_t>(ilit_arg));
                if (nm_v.empty()) {
                    diags_.error(e->loc, "field_name: el tipo '" +
                                             comptime_type_name(tc_, t1) +
                                             "' no tiene campo en indice " +
                                             std::to_string(ilit_arg));
                }
                out_value = emit_strmake_for(nm_v);
                return true;
            }
            if (name == "is_same") {
                const Type t2 = tc_.resolve_type_node(e->type_args[1].get());
                const bool v = comptime_is_same(tc_, t1, t2);
                out_value =
                    emit_const(ir::IrType::BOOL, v ? 1ULL : 0ULL, src_line);
                return true;
            }
            if (name == "is_subtype") {
                const Type t2 = tc_.resolve_type_node(e->type_args[1].get());
                const bool v = comptime_is_subtype(tc_, t1, t2);
                out_value =
                    emit_const(ir::IrType::BOOL, v ? 1ULL : 0ULL, src_line);
                return true;
            }
        }
    }

    // Conjunto de nombres builtin reconocidos.  Si el nombre no esta
    // aqui devolvemos false para que lower_call siga con la ruta
    // generica (CALL a una funcion del usuario).
    // monitor builtins.  Cada uno baja a 1 instruccion bytecode.
    const bool is_wait = (name == "wait");
    const bool is_notify = (name == "notify");
    const bool is_notifyAll = (name == "notifyAll");
    // CPU dispatch (cimiento): consulta runtime de features.
    const bool is_cpu_features = (name == "cpu_features");
    // procesos / IPC builtins.
    const bool is_pid = (name == "pid");
    // Variadicos: vacount() -> lee el param oculto __vacount (numero de args
    // variadicos empaquetados por el caller).
    if (name == "vacount") {
        const ir::IrValueId v = lookup("__vacount");
        if (v == ir::IR_NO_VALUE) {
            error_at(e->loc, "vacount() solo es valido dentro de una funcion "
                             "con un parametro variadico 'T... name'");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        out_value = v;
        return true;
    }
    // argv del script: bajan a getargc/getarg.
    const bool is_args_count = (name == "args_count");
    const bool is_args_get = (name == "args_get");
    // futures builtins.
    // constructor de tipo coleccion primitivo (arraylist, hashmap,
    // hashset, queue, deque, treemap, treeset, stack).  Si find_col_ctor
    // devuelve no-null, el lowering emite CALLN al native_new_fn del
    // plugin vesta_collections con el argumento de capacidad inicial
    // (o sin args para tipos sin default_cap como TreeMap).
    const ColType *col_ctor = find_col_ctor(name);
    const bool is_col_ctor = (col_ctor != nullptr);
    // FFI runtime dinamico: builtins sintaxis-VSH para cargar
    // DLLs y resolver/llamar simbolos en tiempo de ejecucion.
    const bool is_ffi_open = (name == "ffi_open");
    const bool is_ffi_sym = (name == "ffi_sym");
    const bool is_ffi_call = (name == "ffi_call");
    // panic("msg") -> opcode panic con FATAL_USER_ABORT.
    const bool is_panic = (name == "panic");
    // Math builtins (delegan a stdlib/native/math/vesta_math).
    const bool is_math_sqrt = (name == "sqrt");
    const bool is_math_pow = (name == "pow");
    const bool is_math_fabs = (name == "fabs");
    const bool is_math_floor = (name == "floor");
    const bool is_math_ceil = (name == "ceil");
    const bool is_math_round = (name == "round");
    const bool is_math_fmin = (name == "fmin");
    const bool is_math_fmax = (name == "fmax");
    const bool is_math_log = (name == "log");
    const bool is_math_log2 = (name == "log2");
    const bool is_math_log10 = (name == "log10");
    const bool is_math_sin = (name == "sin");
    const bool is_math_cos = (name == "cos");
    const bool is_math_tan = (name == "tan");
    const bool is_math_abs = (name == "abs");
    const bool is_math_imin = (name == "imin");
    const bool is_math_imax = (name == "imax");
    const bool is_math_clamp = (name == "clamp");
    // Math-IR-promote v2.2a: bit ops + new int/float ops.
    const bool is_math_trunc = (name == "trunc");
    const bool is_math_iminu = (name == "iminu");
    const bool is_math_imaxu = (name == "imaxu");
    const bool is_math_ilog2 = (name == "ilog2");
    const bool is_math_popcount = (name == "popcount");
    const bool is_math_clz = (name == "clz");
    const bool is_math_ctz = (name == "ctz");
    const bool is_math_bswap = (name == "bswap");
    const bool is_math_rotl = (name == "rotl");
    const bool is_math_rotr = (name == "rotr");
    const bool is_any_math =
        is_math_sqrt || is_math_pow || is_math_fabs || is_math_floor ||
        is_math_ceil || is_math_round || is_math_fmin || is_math_fmax ||
        is_math_log || is_math_log2 || is_math_log10 || is_math_sin ||
        is_math_cos || is_math_tan || is_math_abs || is_math_imin ||
        is_math_imax || is_math_clamp || is_math_trunc || is_math_iminu ||
        is_math_imaxu || is_math_ilog2 || is_math_popcount || is_math_clz ||
        is_math_ctz || is_math_bswap || is_math_rotl || is_math_rotr;
    // smart pointers builtins (unique<T> y shared<T>).
    const bool is_unique_box = (name == "unique_box");
    const bool is_shared_box = (name == "shared_box");
    const bool is_unique_with = (name == "unique_with");
    const bool is_shared_with = (name == "shared_with");
    // bug6 - gc_box(value): aloja el valor en un bloque GC-managed
    // (vx_gc_alloc_ptr / GC_ALLOCP) y devuelve el host_ptr al box.
    const bool is_gc_box = (name == "gc_box");
    // Borrow builtins: lend/lend_mut son operaciones zero-overhead
    // que devuelven el ptr_of del owner (slot+0).  El borrow checker
    // ya valido las reglas en compile-time, asi que aqui solo emitimos
    // la lectura del puntero.  read_borrow/write_borrow son
    // *p y *p=v respectivamente.
    const bool is_lend = (name == "lend");
    const bool is_lend_mut = (name == "lend_mut");
    const bool is_read_borrow = (name == "read_borrow");
    const bool is_write_borrow = (name == "write_borrow");
    const bool is_move = (name == "move");
    const bool is_get = (name == "ptr_of");
    const bool is_use_count = (name == "use_count");
    // Z.6 builtins: is_shared / share / unshare.
    // Atomicos GENERICOS (width-aware): el ancho sale del pointee del puntero.
    const bool is_atomic_load_g = (name == "atomic_load");
    const bool is_atomic_store_g = (name == "atomic_store");
    const bool is_atomic_cas_g = (name == "atomic_cas");
    const bool is_atomic_add_g = (name == "atomic_add");
    // Atomicos GENERICOS (ancho = pointee del puntero arg 0).  Se resuelve
    // AQUI, temprano, antes que cualquier handler que pudiera retornar antes.
    // Sirven a atomic<T> para 1/2/4/8 bytes (i8..i64, u8..u64, f32, f64, bool,
    // ptr).
    if (is_atomic_load_g || is_atomic_store_g || is_atomic_cas_g ||
        is_atomic_add_g) {
        ir::IrType wt = ir::IrType::I64;
        if (!e->args.empty() && e->args[0]->result_type.pointee)
            wt = ir_type_from_primitive(e->args[0]->result_type.pointee->kind);
        // Los atomicos operan sobre BITS enteros (banco GP + instruccion `lock`
        // del ancho).  Un pointee FLOAT (f32/f64) vive en el banco ZMM: la
        // operacion se hace sobre el entero del MISMO ancho (F32->I32,
        // F64->I64) y el valor cruza con BITCAST puro (misma anchura, mismos
        // bits IEEE), que baja a movd/movq -- cero coste.  Sin esto, un
        // `atomic_store` de f32 guardaba los 32 bits bajos de un patron f64 (=
        // 0).
        const bool is_flt = (wt == ir::IrType::F32 || wt == ir::IrType::F64);
        const ir::IrType iwt = (wt == ir::IrType::F32)   ? ir::IrType::I32
                               : (wt == ir::IrType::F64) ? ir::IrType::I64
                                                         : wt;
        auto emit_bc = [&](ir::IrValueId src, ir::IrType tgt) -> ir::IrValueId {
            ir::IrValueId d = fn_->new_value(tgt);
            ir::IrInstr bc{};
            bc.op = ir::IrOp::BITCAST;
            bc.type = tgt;
            bc.dst = d;
            bc.operands = {src};
            bc.source_line = e->loc.line;
            emit(current_block_, std::move(bc));
            return d;
        };
        if (is_atomic_load_g) {
            if (e->args.size() != 1) {
                error_at(e->loc, "atomic_load: requiere 1 argumento (T*)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
            ir::IrValueId bits = emit_atomic_ld_i64(v_ptr, e->loc.line, iwt);
            out_value =
                is_flt ? emit_bc(bits, wt) : bits; // bits GP -> float ZMM
            return true;
        }
        if (is_atomic_store_g) {
            if (e->args.size() != 2) {
                error_at(e->loc, "atomic_store: requiere 2 argumentos (T*, T)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
            ir::IrValueId v_val = lower_expr(e->args[1].get());
            if (is_flt) {
                // El valor puede llegar como f64 (un literal `5.0`, que es f64
                // por defecto, propagado por la cadena de inline sin
                // re-estrecharse) mientras la celda es f32.  Coaccionar al
                // ancho float REAL de T antes de tomar sus bits.
                v_val = cast_if_needed(v_val, fn_->values[v_val].type, wt,
                                       e->loc.line, true);
                v_val = emit_bc(v_val, iwt); // float ZMM -> bits GP
            }
            emit_atomic_st_i64(v_ptr, v_val, e->loc.line, iwt);
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        if (is_atomic_cas_g) {
            if (e->args.size() != 3) {
                error_at(e->loc,
                         "atomic_cas: requiere 3 argumentos (T*, exp, des)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
            ir::IrValueId v_exp = lower_expr(e->args[1].get());
            ir::IrValueId v_des = lower_expr(e->args[2].get());
            if (is_flt) {
                // comparar/escribir los BITS, no el valor
                v_exp = cast_if_needed(v_exp, fn_->values[v_exp].type, wt,
                                       e->loc.line, true);
                v_des = cast_if_needed(v_des, fn_->values[v_des].type, wt,
                                       e->loc.line, true);
                v_exp = emit_bc(v_exp, iwt);
                v_des = emit_bc(v_des, iwt);
            }
            ir::IrValueId res =
                emit_atomic_cas_i64(v_ptr, v_exp, v_des, e->loc.line, iwt);
            out_value = is_flt ? emit_bc(res, wt) : res; // OLD bits -> float
            return true;
        }
        // is_atomic_add_g.  El delta de un xadd es entero por naturaleza; el
        // .vx nunca invoca atomic_add con float (fetch_add sobre float usa el
        // bucle CAS de arriba).  Se baja como entero del ancho de T.
        if (e->args.size() != 2) {
            error_at(e->loc, "atomic_add: requiere 2 argumentos (T*, delta)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
        const ir::IrValueId v_delta = lower_expr(e->args[1].get());
        out_value = emit_atomic_add_i64(v_ptr, v_delta, e->loc.line, iwt);
        return true;
    }
    // Z.10 builtins: introspeccion + GC placeholder del SharedHeap.
    const bool is_z10_live_count = (name == "shared_heap_live_count");
    const bool is_z10_bytes = (name == "shared_heap_bytes");
    const bool is_z10_gc_collect = (name == "shared_gc_collect");
    // Builtins de string: cada uno baja a una sola instruccion bytecode
    // dedicada (STRLEN, STRGETBYTES, STRRAW, etc.) sin pasar por CALLN.
    /*  MC.15B: alias comptime_* a sus equivalentes runtime str_*
     * cuando aparecen en cuerpos de @Macro lowereados a IR.  El
     * type_checker ya valido el call con el comptime evaluator; aqui
     * solo emitimos el bytecode que la VM ejecutara al invocar el
     * macro lowereado.  Mismo path que el runtime str_* user-facing. */
    const bool is_str_length =
        (name == "str_length" || name == "comptime_strlen");
    const bool is_str_bytes = (name == "str_bytes");
    const bool is_str_cstr = (name == "str_cstr");
    const bool is_str_wstr = (name == "str_wstr");
    const bool is_str_hash = (name == "str_hash");
    const bool is_str_intern = (name == "str_intern");
    const bool is_str_concat =
        (name == "str_concat" || name == "comptime_concat");
    const bool is_str_equals =
        (name == "str_equals" || name == "comptime_streq");
    const bool is_str_make = (name == "str_make");
    const bool is_str_convert = (name == "str_convert");
    /*  MC.15C: aliases comptime adicionales que lowerean a
     * codigo runtime (eliminando rejection en macro pre-validation). */
    const bool is_to_str = (name == "to_str" || name == "comptime_to_str");
    const bool is_chr_b = (name == "chr" || name == "comptime_chr");
    const bool is_ord_b = (name == "ord" || name == "comptime_ord");
    const bool is_substr_b = (name == "substr" || name == "comptime_substr");
    const bool is_gensym_b = (name == "gensym");
    const bool is_repeat_b = (name == "repeat" || name == "comptime_repeat");
    const bool is_replace_b = (name == "replace" || name == "comptime_replace");
    const bool is_contains_b =
        (name == "contains" || name == "comptime_contains");
    const bool is_static_assert_b = (name == "static_assert");
    /* Los de imprimir NO estan en esta lista, y no es un olvido: se atienden
     * arriba del todo, asi que si el flujo llega hasta aqui es que el nombre no
     * era de esa familia. */
    /* Tampoco estan los que se atienden ARRIBA (imprimir, y lo que pide
     * algo al mundo): si el flujo llega hasta aqui, el nombre no era de
     * esas familias. */
    const bool is_any_builtin =
        is_wait || is_notify || is_notifyAll ||
        is_cpu_features || is_pid ||
        is_args_count || is_args_get ||
        is_ffi_open || is_ffi_sym || is_ffi_call || is_panic ||
        is_str_length || is_str_bytes || is_str_cstr || is_str_wstr ||
        is_str_hash || is_str_intern || is_str_concat || is_str_equals ||
        is_str_make || is_str_convert || is_to_str || is_chr_b ||
        is_ord_b || is_substr_b || is_gensym_b || is_repeat_b ||
        is_replace_b || is_contains_b || is_static_assert_b || is_any_math ||
        is_col_ctor || is_unique_box || is_shared_box || is_gc_box ||
        is_unique_with || is_shared_with || is_move || is_get ||
        is_use_count || is_lend || is_lend_mut || is_read_borrow ||
        is_write_borrow || is_z10_live_count || is_z10_bytes ||
        is_z10_gc_collect;
    if (!is_any_builtin) return false;

    // Helper interno para registrar un literal de string en static_data

    // ===== Constructor de coleccion primitiva =====
    // arraylist(N) -> CALLN vcol_alist_new(N), retorno i64 handle.
    // Para tipos sin default_cap (TreeMap/TreeSet) la firma del builtin
    // no toma argumentos; emitimos CALLN con argc=0.
    if (is_col_ctor) {
        std::vector<ir::IrValueId> arg_ids;
        for (auto &a : e->args) {
            arg_ids.push_back(lower_expr(a.get()));
        }
        // Si el ctor tiene default_cap > 0 y el usuario llamo sin args,
        // sintetizamos la cap por defecto.  El type checker valida que
        // siempre haya 1 arg para los ctors con default_cap; pero por
        // seguridad emitimos default cuando el array de args esta vacio.
        if (arg_ids.empty() && col_ctor->default_cap > 0) {
            arg_ids.push_back(emit_const(
                ir::IrType::I64, static_cast<uint64_t>(col_ctor->default_cap),
                e->loc.line));
        }
        out_mod_->register_native_import(COL_NATIVE_LIB,
                                         col_ctor->native_new_fn);
        const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLN;
        ins.type = ir::IrType::I64;
        ins.dst = v_dst;
        ins.func_name =
            std::string(COL_NATIVE_LIB) + ":" + col_ctor->native_new_fn;
        ins.operands = std::move(arg_ids);
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        out_value = v_dst;
        return true;
    }

    // ----- ffi_open(string lit) -----
    // Carga DLL en runtime via opcode dlopen (extended 0x62).  Path
    // siempre como string literal (interned en static_data).  Devuelve
    // handle host como i64.
    if (is_ffi_open) {
        if (e->args.size() != 1 || !e->args[0] ||
            e->args[0]->kind != ast::NodeKind::StringLitExpr) {
            error_at(e->loc, "ffi_open: requiere un string literal con el "
                             "nombre/path de la DLL");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        auto *slit = static_cast<ast::StringLitExpr *>(e->args[0].get());
        // NUL-terminar el path interned: en AOT nativo se baja a
        // LoadLibraryA/dlopen (APIs cstring que leen hasta el NUL).  El
        // path_len sigue siendo el tamano logico (sin NUL); el path VM/JIT usa
        // (addr,len) e ignora el NUL.
        const uint64_t path_idx =
            intern_class_name(*out_mod_, slit->value + std::string(1, '\0'));
        const uint32_t path_len = static_cast<uint32_t>(slit->value.size());
        // raw_asm-elim wave 2: DLOPEN IR op.
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
        ir::IrInstr dl{};
        dl.op = ir::IrOp::DLOPEN;
        dl.type = ir::IrType::I64;
        dl.dst = v_dst;
        dl.operands = {v_path_addr, v_path_len};
        dl.source_line = e->loc.line;
        emit(current_block_, std::move(dl));
        out_value = v_dst;
        return true;
    }

    // ----- ffi_sym(handle, string lit) -----
    // Resuelve simbolo en una DLL cargada.  El handle viene de un SSA
    // value (resultado de ffi_open o expression i64); el name es
    // string literal (interned en static_data).  Devuelve fn_addr i64.
    if (is_ffi_sym) {
        if (e->args.size() != 2 || !e->args[0] || !e->args[1] ||
            e->args[1]->kind != ast::NodeKind::StringLitExpr) {
            error_at(e->loc, "ffi_sym: requiere (i64 handle, string lit name)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_handle = lower_expr(e->args[0].get());
        if (v_handle == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        auto *slit = static_cast<ast::StringLitExpr *>(e->args[1].get());
        // NUL-terminar: en AOT nativo se baja a GetProcAddress/dlsym (cstring).
        const uint64_t name_idx =
            intern_class_name(*out_mod_, slit->value + std::string(1, '\0'));
        const uint32_t name_len = static_cast<uint32_t>(slit->value.size());
        // raw_asm-elim wave 2: DLSYM IR op.
        const ir::IrValueId v_name_addr = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr sl{};
            sl.op = ir::IrOp::STR_LIT_ADDR;
            sl.type = ir::IrType::PTR;
            sl.dst = v_name_addr;
            sl.imm = name_idx;
            sl.source_line = e->loc.line;
            emit(current_block_, std::move(sl));
        }
        const ir::IrValueId v_name_len =
            emit_const(ir::IrType::I64, name_len, e->loc.line);
        const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ds{};
        ds.op = ir::IrOp::DLSYM;
        ds.type = ir::IrType::I64;
        ds.dst = v_dst;
        ds.operands = {v_handle, v_name_addr, v_name_len};
        ds.source_line = e->loc.line;
        emit(current_block_, std::move(ds));
        out_value = v_dst;
        return true;
    }

    // ----- ffi_call(fn, ...args) -----  (variadic 0-12 args)
    // Invoca funcion nativa via puntero (resuelto por ffi_sym/dlsym o
    // pasado como handle).  Calling convention espejo a CALLN estatico:
    // argc en R15, args en R01..R12, retorno en R00.
    //
    // Implementacion: emitir IrInstr CALLN con func_name="__callni__:"
    // y operands=[fn, args...].  El emitter detecta el prefix y emite
    // la secuencia completa (push regs vivos + parallel-move args ->
    // R1..RN + mov r15, N + callni reg_fn + capturar R0 + pop regs).
    // Reusa toda la maquinaria de CALLN para mantener una sola ruta.
    if (is_ffi_call) {
        if (e->args.empty()) {
            error_at(e->loc,
                     "ffi_call: requiere al menos el puntero a funcion");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        if (e->args.size() > 13) {
            error_at(e->loc, "ffi_call: maximo 12 args ademas del puntero");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        std::vector<ir::IrValueId> arg_ids;
        arg_ids.reserve(e->args.size());
        for (auto &a : e->args) {
            arg_ids.push_back(lower_expr(a.get()));
        }
        const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLN;
        ins.type = ir::IrType::I64;
        ins.dst = v_dst;
        ins.func_name = "__callni__:"; // prefix detectado en emitter
        ins.operands = std::move(arg_ids);
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        out_value = v_dst;
        return true;
    }

    // ----- panic("msg") -----
    // dispara FatalError(USER_ABORT, msg).  Capturable con
    // try/catch FatalError; si no hay handler, mata el proceso.
    // Acepta string literal (interna en static_data + emite panic
    // directo) o expresion string-typed (no soportado todavia).
    // Math builtins -> CALLN a vesta_math.dll.  ABI: bits IEEE 754
    // como uint64_t en r1..rN, retorno (bits) en r0.  Para funciones
    // con tipo de retorno float (sqrt, pow, sin, ...), el callee
    // devuelve los bits f64.  Para funciones que devuelven int (abs,
    // imin, imax, clamp), el valor se devuelve como i64 directo.
    if (is_any_math) {
        const std::string lib_math = "stdlib/native/math/vesta_math";

        // Math-IR-promote (raw_asm-elim wave 4): para builtins con IR
        // op nativa (FSQRT/FABS/FMIN/FMAX/FFLOOR/FCEIL/FROUND/FTRUNC),
        // emitir el IR op directamente.  Beneficios:
        //   (a) Constant folding: sqrt(2.0) -> literal compile-time.
        //   (b) Selector JIT puede emitir sqrtsd/andpd/roundsd nativos
        //       (~4 ciclos) en lugar de CALLN (~50ns).
        //   (c) Cross-target: cuando llegue ARM Selector, emitira fsqrt.d
        //       sin tocar el IR.
        // Fallback CALLN sigue activo para transcendentales (log/sin/cos/
        // tan/pow/exp): libm los implementa mejor que cualquier inline.
        auto emit_float_irop = [&](ir::IrOp op, size_t nargs) -> bool {
            if (e->args.size() != nargs) {
                error_at(e->loc, std::string("'") + name +
                                     "': " + std::to_string(nargs) + " arg(s)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            std::vector<ir::IrValueId> ops;
            ops.reserve(nargs);
            for (auto &a : e->args) {
                ir::IrValueId v = lower_expr(a.get());
                if (v == ir::IR_NO_VALUE) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                // Promover f32 a f64 si hace falta (IR ops trabajan en f64).
                const ir::IrType vt = fn_->values[v].type;
                if (vt == ir::IrType::F32) {
                    ir::IrValueId f64v = fn_->new_value(ir::IrType::F64);
                    ir::IrInstr ext{};
                    ext.op = ir::IrOp::F32TOF64;
                    ext.type = ir::IrType::F64;
                    ext.dst = f64v;
                    ext.operands = {v};
                    ext.source_line = e->loc.line;
                    emit(current_block_, std::move(ext));
                    v = f64v;
                } else if (vt != ir::IrType::F64) {
                    // Si el arg no es float, lo dejamos como esta (el
                    // emitter trata bits como i64 o el caller hizo cast).
                }
                ops.push_back(v);
            }
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::F64);
            ir::IrInstr in{};
            in.op = op;
            in.type = ir::IrType::F64;
            in.dst = v_dst;
            in.operands = std::move(ops);
            in.source_line = e->loc.line;
            emit(current_block_, std::move(in));
            out_value = v_dst;
            return true;
        };
        // Math-IR-promote: despachar a IR op directamente para los que
        // tienen instr hardware nativa (target-agnostico).  Beneficios:
        //   (a) FSQRT/FABS/FNEG bajan a bytecode VM nativo (fsqrt/fabs/
        //       fneg, ~5ns) en lugar de CALLN (~50ns).
        //   (b) Para FMIN/FMAX/FFLOOR/FCEIL/FROUND/FTRUNC el bytecode
        //       todavia no tiene opcodes; el IR emitter (ir_emitter.cpp)
        //       tiene un pre-pase que los convierte a CALLN equivalente.
        //   (c) El Selector JIT (futuro) emite sqrtsd/andpd/minsd/roundsd
        //       nativos sin tocar el frontend.
        //   (d) Constant folding (cuando se añada) funciona uniforme.
        if (is_math_sqrt) return emit_float_irop(ir::IrOp::FSQRT, 1);
        if (is_math_fabs) return emit_float_irop(ir::IrOp::FABS, 1);
        if (is_math_fmin) return emit_float_irop(ir::IrOp::FMIN, 2);
        if (is_math_fmax) return emit_float_irop(ir::IrOp::FMAX, 2);
        if (is_math_floor) return emit_float_irop(ir::IrOp::FFLOOR, 1);
        if (is_math_ceil) return emit_float_irop(ir::IrOp::FCEIL, 1);
        if (is_math_round) return emit_float_irop(ir::IrOp::FROUND, 1);
        if (is_math_trunc) return emit_float_irop(ir::IrOp::FTRUNC, 1);

        // Math-IR-promote v2.2a: bit ops + int ops adicionales.
        // Producen i64 (no float).  Lambda paralela a emit_float_irop.
        auto emit_int_irop = [&](ir::IrOp op, size_t nargs) -> bool {
            if (e->args.size() != nargs) {
                error_at(e->loc, std::string("'") + name +
                                     "': " + std::to_string(nargs) + " arg(s)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            std::vector<ir::IrValueId> ops;
            ops.reserve(nargs);
            for (auto &a : e->args) {
                ir::IrValueId v = lower_expr(a.get());
                if (v == ir::IR_NO_VALUE) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                ops.push_back(cast_if_needed(v, fn_->values[v].type,
                                             ir::IrType::I64, e->loc.line));
            }
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr in{};
            in.op = op;
            in.type = ir::IrType::I64;
            in.dst = v_dst;
            in.operands = std::move(ops);
            in.source_line = e->loc.line;
            emit(current_block_, std::move(in));
            out_value = v_dst;
            return true;
        };
        if (is_math_iminu) return emit_int_irop(ir::IrOp::IMINU, 2);
        if (is_math_imaxu) return emit_int_irop(ir::IrOp::IMAXU, 2);
        if (is_math_ilog2) return emit_int_irop(ir::IrOp::ILOG2, 1);
        if (is_math_popcount) return emit_int_irop(ir::IrOp::POPCNT, 1);
        if (is_math_clz) return emit_int_irop(ir::IrOp::CLZ, 1);
        if (is_math_ctz) return emit_int_irop(ir::IrOp::CTZ, 1);
        if (is_math_bswap) return emit_int_irop(ir::IrOp::BYTESWAP, 1);
        if (is_math_rotl) return emit_int_irop(ir::IrOp::ROTL, 2);
        if (is_math_rotr) return emit_int_irop(ir::IrOp::ROTR, 2);
        // Promocion IMIN/IMAX/IABS a IR op tambien (los wires antiguos
        // CALLN siguen activos abajo pero el pre-pase los re-wirea).
        if (is_math_abs) return emit_int_irop(ir::IrOp::IABS, 1);
        if (is_math_imin) return emit_int_irop(ir::IrOp::IMIN, 2);
        if (is_math_imax) return emit_int_irop(ir::IrOp::IMAX, 2);

        // Camino CALLN tradicional para transcendentales
        // (log/exp/sin/cos/tan/pow) e ints (abs/imin/imax/clamp).  libm los
        // implementa mejor que cualquier inline que podamos emitir.
        std::string func_name;
        size_t expected_args = 1;
        ir::IrType ret_ir = ir::IrType::F64;
        ir::IrType arg_ir =
            ir::IrType::I64; // por defecto pasa bits f64 como i64
        bool dst_is_float = true;
        if (is_math_pow) {
            func_name = "vmath_pow";
            expected_args = 2;
        } else if (is_math_log) {
            func_name = "vmath_log";
        } else if (is_math_log2) {
            func_name = "vmath_log2";
        } else if (is_math_log10) {
            func_name = "vmath_log10";
        } else if (is_math_sin) {
            func_name = "vmath_sin";
        } else if (is_math_cos) {
            func_name = "vmath_cos";
        } else if (is_math_tan) {
            func_name = "vmath_tan";
        } else if (is_math_abs) {
            func_name = "vmath_abs";
            ret_ir = ir::IrType::I64;
            dst_is_float = false;
        } else if (is_math_imin) {
            func_name = "vmath_min";
            expected_args = 2;
            ret_ir = ir::IrType::I64;
            dst_is_float = false;
        } else if (is_math_imax) {
            func_name = "vmath_max";
            expected_args = 2;
            ret_ir = ir::IrType::I64;
            dst_is_float = false;
        } else if (is_math_clamp) {
            func_name = "vmath_clamp";
            expected_args = 3;
            ret_ir = ir::IrType::I64;
            dst_is_float = false;
        }
        if (e->args.size() != expected_args) {
            error_at(e->loc, std::string("'") + name + "': " +
                                 std::to_string(expected_args) + " arg(s)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        std::vector<ir::IrValueId> ops;
        ops.reserve(expected_args);
        for (auto &a : e->args) {
            ir::IrValueId v = lower_expr(a.get());
            if (v == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // ABI nativo: vmath_* recibe bits IEEE 754 como uint64_t.
            // Para floats el value YA esta en GP como bits (lower_expr
            // de un f64 produce un i64 en GP); pasamos tal cual via
            // BITCAST (NO cast_if_needed/FTOI, que convertiria VALOR).
            // Para int builtins (abs/imin/imax/clamp) un cast normal
            // i32->i64 es lo correcto.
            const ir::IrType vt = fn_->values[v].type;
            if ((vt == ir::IrType::F64 || vt == ir::IrType::F32) &&
                arg_ir == ir::IrType::I64) {
                if (vt == ir::IrType::F32) {
                    ir::IrValueId f64v = fn_->new_value(ir::IrType::F64);
                    ir::IrInstr ext{};
                    ext.op = ir::IrOp::F32TOF64;
                    ext.type = ir::IrType::F64;
                    ext.dst = f64v;
                    ext.operands = {v};
                    ext.source_line = e->loc.line;
                    emit(current_block_, std::move(ext));
                    v = f64v;
                }
                ir::IrValueId bits = fn_->new_value(ir::IrType::I64);
                ir::IrInstr bc{};
                bc.op = ir::IrOp::BITCAST;
                bc.type = ir::IrType::I64;
                bc.dst = bits;
                bc.operands = {v};
                bc.source_line = e->loc.line;
                emit(current_block_, std::move(bc));
                v = bits;
            } else {
                v = cast_if_needed(v, vt, arg_ir, e->loc.line);
            }
            ops.push_back(v);
        }
        out_mod_->register_native_import(lib_math, func_name);
        const ir::IrValueId dst = fn_->new_value(ret_ir);
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLN;
        ins.type = ret_ir;
        ins.dst = dst;
        ins.func_name = lib_math + ":" + func_name;
        ins.operands = std::move(ops);
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        (void)dst_is_float;
        out_value = dst;
        return true;
    }

    if (is_panic) {
        if (e->args.size() != 1 || !e->args[0] ||
            e->args[0]->kind != ast::NodeKind::StringLitExpr) {
            error_at(e->loc,
                     "panic: requiere un string literal con el mensaje");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        auto *slit = static_cast<ast::StringLitExpr *>(e->args[0].get());
        const uint64_t msg_idx = intern_class_name(*out_mod_, slit->value);
        const uint32_t msg_len = static_cast<uint32_t>(slit->value.size());
        // Sprint 6.D: panic via IR op puro (STR_LIT_ADDR + CONST + PANIC).
        // AOT.2.d: en native el msg vive en static_data y el HOST_LEAF lo baja
        // a una ref .rodata.
        //
        // El indice viaja como NUMERO (`imm`), nunca como el nombre textual
        // "s_<idx>" de un LABEL_ADDR: al mergear modulos, el pool de
        // static_data se concatena y se deduplica, y los dos pases que
        // renumeran (compiler_project.cpp) reescriben `STR_LIT_ADDR.imm` --
        // no el `func_name` de un LABEL_ADDR.  Un `panic()` dentro de un
        // modulo IMPORTADO conservaba su indice local y acababa apuntando al
        // slot que ese indice ocupa en el modulo consumidor (p.ej. el storage
        // de una variable global, que ademas vive en `gdata` y no en `code`)
        // -> "RelocationError: simbolo no resuelto: code.s_0".
        ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
        // Solo en native el literal es memoria del host; en la VM el mensaje
        // vive en su espacio de direcciones y PANIC lo lee de ahi.
        if (native_poo_) fn_->values[v_addr].is_host_ptr = true;
        {
            ir::IrInstr sa{};
            sa.op = ir::IrOp::STR_LIT_ADDR;
            sa.type = ir::IrType::PTR;
            sa.dst = v_addr;
            sa.imm = msg_idx;
            sa.source_line = e->loc.line;
            emit(current_block_, std::move(sa));
        }
        const ir::IrValueId v_len = emit_const(
            ir::IrType::I64, static_cast<uint64_t>(msg_len), e->loc.line);
        ir::IrInstr p{};
        p.op = ir::IrOp::PANIC;
        p.type = ir::IrType::VOID;
        p.dst = ir::IR_NO_VALUE;
        p.operands = {v_addr, v_len};
        p.source_line = e->loc.line;
        emit(current_block_, std::move(p));
        block_terminated_ =
            true; // panic es terminador (no retorna salvo via catch)
        out_value = ir::IR_NO_VALUE;
        return true;
    }

    /*  MC.15C: builtins comptime aliasados a codigo runtime.
     * Cuando aparecen en cuerpos de @Macro lowereados a IR, se
     * compilan a una secuencia de bytecode equivalente al AST eval. */

    if (is_to_str) {
        /* to_str(int) -> string.  Reusa el helper
         * stringify_primitive_via_native con vio_int_to_vmbuf. */
        if (e->args.size() != 1) {
            error_at(e->loc, "to_str: se esperaba 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_val = lower_expr(e->args[0].get());
        if (v_val == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        out_value = stringify_primitive_via_native(v_val, "vio_int_to_vmbuf",
                                                   e->loc.line);
        return true;
    }

    if (is_chr_b) {
        /* chr(codepoint) -> string.  Reusa vio_char_to_vmbuf
         * (codepoint -> UTF-8 bytes -> STRMAKE). */
        if (e->args.size() != 1) {
            error_at(e->loc, "chr: se esperaba 1 argumento (codepoint)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_cp = lower_expr(e->args[0].get());
        if (v_cp == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        out_value = stringify_primitive_via_native(v_cp, "vio_char_to_vmbuf",
                                                   e->loc.line);
        return true;
    }

    if (is_ord_b) {
        /* ord(s) -> u64.  Devuelve el primer codepoint del string.
         * Fast path ASCII: emit strraw + LOAD u8 (host).  Para
         * multi-byte UTF-8 retorna solo el primer byte (lead byte);
         * el caller puede decodear si necesita el codepoint real. */
        if (e->args.size() != 1) {
            error_at(e->loc, "ord: se esperaba 1 argumento (string)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_str = lower_expr(e->args[0].get());
        if (v_str == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        /* strraw r_raw, r_str  -> host_ptr a bytes */
        ir::IrValueId v_raw = emit_strraw(v_str, e->loc.line);
        /* LOAD.u8 al primer byte (host).  El IR LOAD con is_host_ptr
         * en la fuente emite `movh` automaticamente. */
        ir::IrValueId v_byte = fn_->new_value(ir::IrType::U64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::U8;
            ld.dst = v_byte;
            ld.operands = {v_raw};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
        }
        out_value = v_byte;
        return true;
    }

    if (is_substr_b) {
        /* substr(s, start, len) -> string.  Empaqueta start+len en
         * un u64 (hi<<32 | lo) y emite strslice. */
        if (e->args.size() != 3) {
            error_at(e->loc,
                     "substr: se esperaba 3 argumentos (string, start, len)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_str = lower_expr(e->args[0].get());
        const ir::IrValueId v_start = lower_expr(e->args[1].get());
        const ir::IrValueId v_len = lower_expr(e->args[2].get());
        if (v_str == ir::IR_NO_VALUE || v_start == ir::IR_NO_VALUE ||
            v_len == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        /* Pack: r_range = (start << 32) | len */
        ir::IrValueId v_shifted = fn_->new_value(ir::IrType::U64);
        {
            ir::IrInstr sh{};
            sh.op = ir::IrOp::SHL;
            sh.type = ir::IrType::U64;
            sh.dst = v_shifted;
            sh.operands = {v_start,
                           emit_const(ir::IrType::U64, 32, e->loc.line)};
            sh.source_line = e->loc.line;
            emit(current_block_, std::move(sh));
        }
        ir::IrValueId v_range = fn_->new_value(ir::IrType::U64);
        {
            ir::IrInstr orop{};
            orop.op = ir::IrOp::OR;
            orop.type = ir::IrType::U64;
            orop.dst = v_range;
            orop.operands = {v_shifted, v_len};
            orop.source_line = e->loc.line;
            emit(current_block_, std::move(orop));
        }
        ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr sl{};
            sl.op = ir::IrOp::STRSLICE;
            sl.type = ir::IrType::I64;
            sl.dst = v_dst;
            sl.operands = {v_str, v_range};
            sl.source_line = e->loc.line;
            sl.is_call_site = true;
            emit(current_block_, std::move(sl));
        }
        out_value = v_dst;
        return true;
    }

    if (is_static_assert_b) {
        /*  MC.20: `static_assert(cond, msg)` se baja a CALLN
         * a la virtual kVestaIoLib `vesta_comptime:static_assert`.  El fn
         * recibe (cond_i64, msg_cstr) y emite diagnostic error si
         * cond es 0.  Cuando el macro corre via VM en compile time,
         * la check se ejecuta tambien en compile time -- mismo
         * resultado que el AST eval inline. */
        if (e->args.size() != 2) {
            error_at(e->loc, "static_assert: se esperaba 2 args (cond, msg)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_cond = lower_expr(e->args[0].get());
        if (v_cond == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        /* msg: cualquier string comptime-evaluable, no solo un literal.  El
         * type checker ya lo admite explicitamente -- una `const string` con el
         * mensaje, o una concatenacion -- porque exigir el literal obliga a
         * repetir el mismo texto en cada assert.  Exigirlo AQUI contradecia esa
         * regla: el mismo assert pasaba el chequeo y luego se rechazaba al
         * bajarlo.  Va como host_ptr al buffer estable de static_data,
         * NUL-terminado por construccion. */
        const ast::Expr *msg_e = e->args[1].get();
        std::string msg_text;
        const auto *slit =
            (msg_e && msg_e->kind == ast::NodeKind::StringLitExpr)
                ? static_cast<const ast::StringLitExpr *>(msg_e)
                : nullptr;
        if (slit && !slit->is_interpolated()) {
            msg_text = slit->value;
        } else if (ComptimeEvalResult mv =
                       comptime_eval_expr(tc_, const_cast<ast::Expr *>(msg_e));
                   mv.ok && mv.is_str) {
            msg_text = mv.str;
        } else {
            error_at(e->loc,
                     "static_assert: el msg debe ser un string "
                     "comptime-evaluable (un literal, una 'const string' o una "
                     "concatenacion de ambos)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        /* El mensaje viaja como (direccion, longitud) del espacio de la VM y
         * el helper lo lee con la API del proceso, igual que cualquier otro
         * nativo (`vio_print` y companyia).  Antes se le pasaba la direccion a
         * secas y el helper la trataba como puntero del anfitrion: un numero
         * como 0x27050 que al leerse se llevaba el proceso por delante.  Nunca
         * se habia notado porque esta ruta no llegaba a ejecutarse -- la
         * asercion se descartaba siempre. */
        std::vector<uint8_t> bytes(msg_text.begin(), msg_text.end());
        const uint64_t idx = out_mod_->intern_static_data(std::move(bytes));
        ir::IrValueId v_msg = fn_->new_value(ir::IrType::PTR);
        if (native_poo_) fn_->values[v_msg].is_host_ptr = true;
        {
            ir::IrInstr is{};
            is.op = ir::IrOp::STR_LIT_ADDR;
            is.type = ir::IrType::PTR;
            is.dst = v_msg;
            is.imm = idx;
            is.source_line = e->loc.line;
            emit(current_block_, std::move(is));
        }
        const ir::IrValueId v_len =
            emit_const(ir::IrType::I64, static_cast<uint64_t>(msg_text.size()),
                       e->loc.line);
        const ir::IrValueId v_proc = emit_getproc(e->loc.line);
        /* Corre AL COMPILAR: comprueba la condicion y, si falla, corta la
         * compilacion con el mensaje.  Lo unico que toca del programa es leer
         * ese mensaje (tercer argumento), que es un literal.
         *
         * Lo que hace el codigo comptime son efectos sobre la COMPILACION, no
         * sobre el programa compilado; de ahi que no haya nada mas que declarar
         * aunque aborte. */
        {
            ir::IrNativeEffects fx;
            fx.declarados = true;
            fx.comptime = true;
            fx.lee_apuntado = 1u << 2; // el mensaje
            out_mod_->register_native_import("vesta_comptime", "static_assert",
                                             fx);
        }
        ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
        ir::IrInstr cl{};
        cl.op = ir::IrOp::CALLN;
        cl.type = ir::IrType::I64;
        cl.dst = v_dst;
        cl.func_name = "vesta_comptime:static_assert";
        cl.operands = {v_proc, v_cond, v_msg, v_len};
        cl.source_line = e->loc.line;
        emit(current_block_, std::move(cl));
        /* Una asercion incumplida CORTA la ejecucion aqui mismo.
         *
         * El helper devuelve el veredicto y antes se ignoraba, con lo que el
         * cuerpo seguia corriendo sobre datos que ya se sabian invalidos.  El
         * corte es un `panic` y no un `hlt`: lleva el mensaje, construye la
         * traza de llamadas y -- lo que de verdad importa -- se puede capturar
         * con `try`/`catch` desde el propio codigo comptime, que un `hlt` no
         * permitiria. */
        const ir::IrBlockId sa_fail = fn_->new_block("assert_fail");
        const ir::IrBlockId sa_cont = fn_->new_block("assert_ok");
        {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR_COND;
            br.operands.push_back(v_dst);
            br.target_block = sa_fail; // != 0 -> incumplida
            br.false_block = sa_cont;  // == 0 -> sigue
            br.source_line = e->loc.line;
            emit(current_block_, std::move(br));
        }
        fn_->blocks[current_block_].succs.push_back(sa_fail);
        fn_->blocks[current_block_].succs.push_back(sa_cont);
        {
            /* PANIC lee el mensaje de la memoria de la VM, igual que el helper:
             * se reusan las mismas direccion y longitud. */
            ir::IrInstr p{};
            p.op = ir::IrOp::PANIC;
            p.type = ir::IrType::VOID;
            p.dst = ir::IR_NO_VALUE;
            p.operands = {v_msg, v_len};
            p.source_line = e->loc.line;
            emit(sa_fail, std::move(p));
        }
        current_block_ = sa_cont;
        out_value = v_dst;
        return true;
    }

    if (is_gensym_b) {
        /* gensym() -> u64.  Counter incrementado en cada call.
         * Implementado via CALLN a vio_gensym() en el plugin
         * vesta_io que mantiene un counter estatico. */
        if (!e->args.empty()) {
            error_at(e->loc, "gensym: no acepta argumentos");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        out_mod_->register_native_import(
            std::string("stdlib/native/io/vesta_io"), "vio_gensym");
        ir::IrValueId v_dst = fn_->new_value(ir::IrType::U64);
        ir::IrInstr cl{};
        cl.op = ir::IrOp::CALLN;
        cl.type = ir::IrType::U64;
        cl.dst = v_dst;
        cl.func_name = "stdlib/native/io/vesta_io:vio_gensym";
        cl.operands = {};
        cl.source_line = e->loc.line;
        emit(current_block_, std::move(cl));
        out_value = v_dst;
        return true;
    }

    if (is_repeat_b || is_replace_b || is_contains_b) {
        /*  MC.15D: builtins de string que requieren acceso a
         * los bytes RAW de StringObjects (via STRRAW) y un buffer
         * destino en vm_mem.  Layout comun:
         *   1. Resolver SSA values de cada arg (string -> handle).
         *      AUTO-PROMOCION: literals string como `"{a}"` no son
         *      StringObjects; los promovemos via STRMAKE antes de
         *      hacer STRRAW.  Sin esto, STRRAW recibe un raw ptr a
         *      static_data y devuelve garbage.  Mismo patron que
         *      str_concat / str_equals.
         *   2. Para cada string arg: emitir STRRAW + STRGETBYTES
         *      para obtener host_ptr + length.  Pasar host_ptr como
         *      vm_addr al native (que internamente lo trata como
         *      direccion VM via vm_read_bytes).
         *
         * NOTA: STRRAW devuelve host_ptr, no vm_addr.  Pero los
         * helpers usan `vm_read_bytes` que toma direcciones VM.
         * Para evitar confusion, copiamos cada string a un buffer
         * VM via ALLOCA + copia byte-por-byte... mucho overhead.
         *
         * Alternativa: el helper acepta DIRECTAMENTE el host_ptr
         * (uint64) y lo dereferencea como tal.  Re-disenamos los
         * natives para tomar host_ptr en lugar de vm_addr.  Para
         * mantener consistencia con vio_*_to_vmbuf, los repeat/
         * replace todavia usan vm_addr para el DESTINO; el caller
         * debe pasar un buffer ALLOCA fresco.
         *
         * Plan v1 simplificado: TODOS los args string se materializan
         * a buffer VM via ALLOCA + write.  Costoso para strings
         * grandes pero correcto.  Optimizable despues. */

        // Helper para auto-promote string literals a StringObjects
        // antes de aplicar strraw.  Mismo patron que en str_concat/equals.
        auto coerce_str_arg = [&](ast::Expr *ex) -> ir::IrValueId {
            if (ex && ex->kind == ast::NodeKind::StringLitExpr) {
                auto *sl = static_cast<ast::StringLitExpr *>(ex);
                return lower_string_literal_to_string_object(sl);
            }
            return lower_expr(ex);
        };

        auto materialize_str_to_vmbuf =
            [&](ir::IrValueId v_str,
                int ln) -> std::pair<ir::IrValueId, ir::IrValueId> {
            /* Returns (vm_addr, byte_len).  Aloca buffer VM,
             * llama STRRAW + STRGETBYTES, copia bytes a buffer VM. */
            ir::IrValueId v_raw = emit_strraw(v_str, ln);
            ir::IrValueId v_byte_len = emit_strgetbytes(v_str, ln);
            /* v_raw es host_ptr -- los helpers nativos lo aceptan
             * directamente via `(void *)(uint64_t)host_ptr` y
             * leen con memcpy.  Pero g_api->vm_read_bytes toma
             * VM address, no host_ptr.  Para usar vm_read_bytes
             * necesitamos un VM address.
             *
             * Workaround: ya que los helpers necesitan VM address,
             * vamos a alocar un buffer en VM (ALLOCA) y copiar via
             * un nuevo intrinsic 'memcpyh_to_v' que copia desde
             * host_ptr a vm_mem.  PERO ese intrinsic no existe.
             *
             * Solucion simple: cambiar los helpers nativos para
             * que tomen host_ptr.  Asi pasamos v_raw directo. */
            return {v_raw, v_byte_len};
        };

        if (is_repeat_b) {
            if (e->args.size() != 2) {
                error_at(e->loc,
                         "repeat: se esperaba 2 argumentos (string, n)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_str = coerce_str_arg(e->args[0].get());
            const ir::IrValueId v_n = lower_expr(e->args[1].get());
            if (v_str == ir::IR_NO_VALUE || v_n == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto [v_src_addr, v_src_len] =
                materialize_str_to_vmbuf(v_str, e->loc.line);
            /* Aloca buffer destino (max 16 MB).  Tamano runtime no
             * conocido en compile-time; reservamos ALLOCA grande
             * (64 KB) como cap razonable.  El helper devuelve la
             * longitud escrita y abortara con 0 si excede 16 MB. */
            ir::IrValueId v_dst_buf = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr al{};
                al.op = ir::IrOp::ALLOCA;
                al.type = ir::IrType::I8;
                al.dst = v_dst_buf;
                al.imm = 65536;
                al.source_line = e->loc.line;
                emit(current_block_, std::move(al));
            }
            const ir::IrValueId v_proc = emit_getproc(e->loc.line);
            out_mod_->register_native_import("stdlib/native/io/vesta_io",
                                             "vstr_repeat_to_vmbuf");
            ir::IrValueId v_len = fn_->new_value(ir::IrType::U64);
            {
                ir::IrInstr cl{};
                cl.op = ir::IrOp::CALLN;
                cl.type = ir::IrType::U64;
                cl.dst = v_len;
                cl.func_name = "stdlib/native/io/vesta_io:vstr_repeat_to_vmbuf";
                cl.operands = {v_proc, v_dst_buf, v_src_addr, v_src_len, v_n};
                cl.source_line = e->loc.line;
                emit(current_block_, std::move(cl));
            }
            /* STRMAKE desde el buffer dst. */
            ir::IrValueId v_h = emit_strmake(v_dst_buf, v_len, e->loc.line);
            out_value = v_h;
            return true;
        }

        if (is_contains_b) {
            if (e->args.size() != 2) {
                error_at(
                    e->loc,
                    "contains: se esperaba 2 argumentos (string, substring)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_hay = coerce_str_arg(e->args[0].get());
            const ir::IrValueId v_needle = coerce_str_arg(e->args[1].get());
            if (v_hay == ir::IR_NO_VALUE || v_needle == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto [v_h_addr, v_h_len] =
                materialize_str_to_vmbuf(v_hay, e->loc.line);
            auto [v_n_addr, v_n_len] =
                materialize_str_to_vmbuf(v_needle, e->loc.line);
            const ir::IrValueId v_proc = emit_getproc(e->loc.line);
            out_mod_->register_native_import("stdlib/native/io/vesta_io",
                                             "vstr_contains");
            ir::IrValueId v_dst = fn_->new_value(ir::IrType::BOOL);
            {
                ir::IrInstr cl{};
                cl.op = ir::IrOp::CALLN;
                cl.type = ir::IrType::BOOL;
                cl.dst = v_dst;
                cl.func_name = "stdlib/native/io/vesta_io:vstr_contains";
                cl.operands = {v_proc, v_h_addr, v_h_len, v_n_addr, v_n_len};
                cl.source_line = e->loc.line;
                emit(current_block_, std::move(cl));
            }
            out_value = v_dst;
            return true;
        }

        if (is_replace_b) {
            if (e->args.size() != 3) {
                error_at(
                    e->loc,
                    "replace: se esperaba 3 argumentos (string, from, to)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Auto-promote string literals a StringObjects.  Sin esto,
            // un literal como `"{a}"` se pasa como raw static_data ptr
            // a STRRAW que lo trata como GcHandle invalido -> garbage.
            const ir::IrValueId v_src = coerce_str_arg(e->args[0].get());
            const ir::IrValueId v_from = coerce_str_arg(e->args[1].get());
            const ir::IrValueId v_to = coerce_str_arg(e->args[2].get());
            if (v_src == ir::IR_NO_VALUE || v_from == ir::IR_NO_VALUE ||
                v_to == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto [v_src_addr, v_src_len] =
                materialize_str_to_vmbuf(v_src, e->loc.line);
            auto [v_from_addr, v_from_len] =
                materialize_str_to_vmbuf(v_from, e->loc.line);
            auto [v_to_addr, v_to_len] =
                materialize_str_to_vmbuf(v_to, e->loc.line);
            /* Buffer destino (64 KB ALLOCA). */
            ir::IrValueId v_dst_buf = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr al{};
                al.op = ir::IrOp::ALLOCA;
                al.type = ir::IrType::I8;
                al.dst = v_dst_buf;
                al.imm = 65536;
                al.source_line = e->loc.line;
                emit(current_block_, std::move(al));
            }
            const ir::IrValueId v_proc = emit_getproc(e->loc.line);
            out_mod_->register_native_import("stdlib/native/io/vesta_io",
                                             "vstr_replace_to_vmbuf");
            ir::IrValueId v_len = fn_->new_value(ir::IrType::U64);
            {
                ir::IrInstr cl{};
                cl.op = ir::IrOp::CALLN;
                cl.type = ir::IrType::U64;
                cl.dst = v_len;
                cl.func_name =
                    "stdlib/native/io/vesta_io:vstr_replace_to_vmbuf";
                cl.operands = {v_proc,      v_dst_buf,  v_src_addr, v_src_len,
                               v_from_addr, v_from_len, v_to_addr,  v_to_len};
                cl.source_line = e->loc.line;
                emit(current_block_, std::move(cl));
            }
            ir::IrValueId v_h = emit_strmake(v_dst_buf, v_len, e->loc.line);
            out_value = v_h;
            return true;
        }
    }

    // ----- builtins de string -----
    // Cada uno se baja a un solo opcode bytecode mediante RAW_ASM
    // con substitucion {dst}/{src0}/{src1}.  Cero overhead vs .vel
    // crudo; el regalloc decide los registros.
    // Vesta Embed Inc 0: en native_poo_ el `string` es value-type
    // {ptr,len,cap}.  s.length() -> LOAD len@[slot+8]; s.cstr() ->
    // LOAD ptr@[slot+0] (ya nul-terminado).  No emitimos STRLEN/STRRAW
    // (RUNTIME_DEPENDENT en AOT).  Solo length/cstr en Inc 0; el resto
    // (bytes/hash/intern/wstr) sigue su path normal (no se prueba en AOT).
    // Vesta Embed Inc 0/5/6: en native_poo_ el value-string {ptr,len,cap}
    // resuelve length/bytes/cstr/wstr SIN STRMAKE/STRLEN/STRRAW
    // (RUNTIME_DEPENDENT en AOT).  Inc 6 (encoding UTF-8):
    //   .length() -> conteo de CODE-POINTS (UTF-8).
    //   .bytes()  -> conteo de BYTES (el len crudo del repr).
    //   .cstr()   -> u8* UTF-8 NUL-terminado (Win32 *A / FFI).
    //   .wstr()   -> u16* UTF-16LE NUL-terminado (Win32 *W).
    // Plegado en compile-time: `str_cstr("lit")` / `str_wstr("lit")` sobre un
    // literal no interpolado se resuelve AQUI.  Se transcodifica el texto, se
    // interna como blob en memoria host y se devuelve su direccion: sin
    // STRMAKE, sin STRCONV y sin objeto GC.  Vale en interp, JIT y AOT porque
    // el resultado es una direccion host, igual que la que devolvian esos
    // builtins.  Si el texto no es plegable, se sigue por el camino normal.
    if ((is_str_cstr || is_str_wstr) && e->args.size() == 1 && e->args[0]) {
        const std::string *txt = nullptr;
        ast::Expr *ae = e->args[0].get();
        if (ae->kind == ast::NodeKind::StringLitExpr &&
            !static_cast<ast::StringLitExpr *>(ae)->is_interpolated()) {
            txt = &static_cast<ast::StringLitExpr *>(ae)->value;
        } else if (ae->kind == ast::NodeKind::IdentExpr) {
            // Literal alcanzable por nombre (`const string p = "x"`).
            auto it =
                const_str_locals_.find(static_cast<ast::IdentExpr *>(ae)->name);
            if (it != const_str_locals_.end()) txt = &it->second;
        }
        if (txt) {
            const ir::IrValueId v_blob =
                emit_folded_string_blob(*txt, is_str_wstr ? 3 : 2, e->loc.line);
            if (v_blob != ir::IR_NO_VALUE) {
                out_value = v_blob;
                return true;
            }
        }
    }

    // La longitud de un LITERAL se sabe al compilar, asi que se pliega: es una
    // constante, no algo que haya que averiguar.  Sin esto se construia un
    // StringObject en ejecucion solo para leerle la longitud y tirarlo -- una
    // alocacion por cada `"lit".bytes()`, y en un modulo freestanding (vx_io)
    // eso no se puede pagar, de modo que la unica salida era contar los bytes a
    // mano al escribir el codigo.  Contarlos a mano ya habia fallado: el
    // mensaje de unwrap-null pasaba 54 donde el literal mide 56.
    //
    // Vale para los tres modos: el numero es el mismo se ejecute donde se
    // ejecute.
    if ((is_str_length || is_str_bytes) && e->args.size() == 1) {
        const ast::Expr *ae0 = e->args[0].get();
        if (ae0 && ae0->kind == ast::NodeKind::StringLitExpr &&
            !static_cast<const ast::StringLitExpr *>(ae0)->is_interpolated()) {
            const std::string &lit =
                static_cast<const ast::StringLitExpr *>(ae0)->value;
            uint64_t n = 0;
            if (is_str_bytes) {
                n = static_cast<uint64_t>(lit.size());
            } else {
                // Puntos de codigo: los bytes de continuacion de UTF-8
                // (10xxxxxx) no cuentan, son la cola del anterior.
                for (unsigned char b : lit)
                    if ((b & 0xC0) != 0x80) ++n;
            }
            out_value = emit_const(ir::IrType::I64, n, e->loc.line);
            return true;
        }
    }

    if (native_poo_ &&
        (is_str_length || is_str_bytes || is_str_cstr || is_str_wstr) &&
        e->args.size() == 1) {
        ast::Expr *ae = e->args[0].get();
        // Literal directo `str_length("x")`: raro en native; resolver con
        // el repr construido (correcto pero aloca un buffer descartable).
        ir::IrValueId v_slot;
        if (ae && ae->kind == ast::NodeKind::StringLitExpr &&
            !static_cast<ast::StringLitExpr *>(ae)->is_interpolated()) {
            v_slot = build_native_string_from_literal(
                static_cast<ast::StringLitExpr *>(ae), e->loc.line);
        } else {
            v_slot = lower_expr(ae);
        }
        if (v_slot == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        if (is_str_bytes) {
            // String Inc 5 (SSO): len de BYTES via accesor flag-aware.
            out_value = emit_native_str_len(v_slot, e->loc.line);
        } else if (is_str_length) {
            // Inc 6: code-points (UTF-8).  data_ptr + byte_len -> cplen.
            // Para ASCII == byte_len (sin regresion en los tests ASCII).
            ir::IrValueId v_ptr = emit_native_str_data_ptr(v_slot, e->loc.line);
            ir::IrValueId v_blen = emit_native_str_len(v_slot, e->loc.line);
            out_value = emit_native_str_cplen(v_ptr, v_blen, e->loc.line);
        } else if (is_str_cstr) {
            // String Inc 5 (SSO): data_ptr flag-aware (SSO -> &slot ya
            // nul-terminado; HEAP -> ptr@0).
            out_value = emit_native_str_data_ptr(v_slot, e->loc.line);
        } else {
            // is_str_wstr -- Inc 6: UTF-16LE para Win32 *W.
            ir::IrValueId v_ptr = emit_native_str_data_ptr(v_slot, e->loc.line);
            ir::IrValueId v_blen = emit_native_str_len(v_slot, e->loc.line);
            out_value = emit_native_str_to_utf16(v_ptr, v_blen, e->loc.line);
        }
        return true;
    }
    if (is_str_length || is_str_bytes || is_str_cstr || is_str_wstr ||
        is_str_hash || is_str_intern) {
        if (e->args.size() != 1) {
            error_at(e->loc, std::string("'") + name + "': 1 arg");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // coerce string literal (PTR) a StringObject (STRING
        // handle) inline via STRMAKE.  Sin esto pasar un literal directo
        // a str_cstr("wb") emitia STRRAW sobre el ptr raw del literal en
        // static_data, retornando garbage.  Mismo patron que fix3
        // hace en lower_call para args de funciones top-level.
        ast::Expr *ae = e->args[0].get();
        ir::IrValueId v_str;
        if (ae && ae->kind == ast::NodeKind::StringLitExpr) {
            auto *sl = static_cast<ast::StringLitExpr *>(ae);
            // Tanto literales puros como interpolados: el helper
            // construye el StringObject correcto.
            v_str = lower_string_literal_to_string_object(sl);
        } else {
            v_str = lower_expr(ae);
        }
        if (v_str == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // wstr requiere strconv UTF16 + strraw (2 ops).
        if (is_str_wstr) {
            // strconv(s, ENC_UTF16=3) + strraw -> host_ptr a wchar_t* para
            // Win32 *W.
            ir::IrValueId v_conv =
                emit_strconv(v_str, /*enc=UTF16*/ 3, e->loc.line);
            ir::IrValueId v_raw = emit_strraw(v_conv, e->loc.line);
            out_value = v_raw;
            return true;
        }
        // Resto: 1 sola instruccion bytecode mediante IR ops dedicados.
        if (is_str_length) {
            ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr ins{};
            ins.op = ir::IrOp::STRLEN;
            ins.type = ir::IrType::I64;
            ins.dst = v_dst;
            ins.operands = {v_str};
            ins.source_line = e->loc.line;
            emit(current_block_, std::move(ins));
            out_value = v_dst;
        } else if (is_str_bytes) {
            out_value = emit_strgetbytes(v_str, e->loc.line);
        } else if (is_str_cstr) {
            out_value = emit_strraw(v_str, e->loc.line);
        } else if (is_str_hash) {
            ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr ins{};
            ins.op = ir::IrOp::STRHASH;
            ins.type = ir::IrType::I64;
            ins.dst = v_dst;
            ins.operands = {v_str};
            ins.source_line = e->loc.line;
            emit(current_block_, std::move(ins));
            out_value = v_dst;
        } else {
            // str_intern: aloca nuevo StringObject canonical o reusa pool.
            // Retorna GcHandle (no host_ptr), por eso no is_gc_object.
            ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr ins{};
            ins.op = ir::IrOp::STRINTERN;
            ins.type = ir::IrType::I64;
            ins.dst = v_dst;
            ins.operands = {v_str};
            ins.is_call_site = true;
            ins.source_line = e->loc.line;
            emit(current_block_, std::move(ins));
            out_value = v_dst;
        }
        return true;
    }

    if (is_str_concat || is_str_equals) {
        if (e->args.size() != 2) {
            error_at(e->loc, std::string("'") + name + "': 2 args");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // C-3: ruteo del builtin str_concat/str_equals al override del
        // usuario (@StringConcat / @StringEq).  Solo el builtin runtime
        // real, NO el alias comptime_concat (que vive en compile-time).
        if (name == "str_concat" && !string_concat_override_.empty()) {
            out_value = emit_string_override_call(
                string_concat_override_, e->args[0].get(), e->args[1].get(),
                ir::IrType::I64, /*negate=*/false, e->loc.line);
            return true;
        }
        if (name == "str_equals" && !string_eq_override_.empty()) {
            out_value = emit_string_override_call(
                string_eq_override_, e->args[0].get(), e->args[1].get(),
                ir::IrType::BOOL, /*negate=*/false, e->loc.line);
            return true;
        }
        // Vesta Embed Inc 1: en native_poo_ str_concat(a, b) == `a + b`
        // (value-string).  Mismo lowering: buffer nuevo owned + copia de
        // ambos.  str_equals (cmp) es Inc 4 -> sigue su path normal.
        if (native_poo_ && is_str_concat) {
            auto build_native_operand = [&](ast::Expr *ex,
                                            bool &is_temp) -> ir::IrValueId {
                if (ex && ex->kind == ast::NodeKind::StringLitExpr &&
                    !static_cast<ast::StringLitExpr *>(ex)->is_interpolated()) {
                    is_temp = true;
                    return build_native_string_from_literal(
                        static_cast<ast::StringLitExpr *>(ex), e->loc.line);
                }
                // Concat anidado (`a + b + c`): un operando que es a su vez un
                // `+` de strings produjo un buffer owned SIN RAII (resultado de
                // expresion, no ligado a variable) -> es TEMPORAL: hay que
                // liberarlo tras copiar sus bytes.  Sin esto, el intermedio
                // (a+b) fuga.  Los IdentExpr (variables) NO se marcan temp: su
                // RAII los libera al exit del scope dueno (no doble-free).
                if (ex && ex->kind == ast::NodeKind::BinaryExpr &&
                    static_cast<ast::BinaryExpr *>(ex)->op == ast::BinOp::Add &&
                    ex->result_type.kind == PrimitiveKind::STRING) {
                    is_temp = true;
                    return lower_expr(ex);
                }
                // Cast (string)<char>: produce un slot value-string owned
                // SIN RAII (resultado de expresion).  Es TEMPORAL: hay que
                // liberar su buffer tras copiar los bytes.  Sin esto el
                // buffer del cast fuga.
                if (ex && ex->kind == ast::NodeKind::CastExpr &&
                    ex->result_type.kind == PrimitiveKind::STRING) {
                    is_temp = true;
                    return lower_expr(ex);
                }
                is_temp = false;
                return lower_expr(ex);
            };
            bool a_temp = false, b_temp = false;
            ir::IrValueId v_na = build_native_operand(e->args[0].get(), a_temp);
            ir::IrValueId v_nb = build_native_operand(e->args[1].get(), b_temp);
            if (v_na == ir::IR_NO_VALUE || v_nb == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId v_res =
                build_native_string_concat(v_na, v_nb, e->loc.line);
            // Inc 5 (SSO): liberar operandos temporales SOLO si HEAP.
            if (a_temp) emit_native_str_free_if_heap(v_na, e->loc.line);
            if (b_temp) emit_native_str_free_if_heap(v_nb, e->loc.line);
            out_value = v_res;
            return true;
        }
        // Vesta Embed Inc 4 (builtin): en native_poo_ str_equals(a, b) usa el
        // mismo helper native __vx_strcmp que el operador `==` (value-string,
        // CERO GC), en vez de STRCMP (StringObject GC).  Devuelve bool (==0).
        if (native_poo_ && is_str_equals) {
            // Extrae (ptr, len) de cada operando como el operador == (literal
            // -> .rodata + CONST len; var/concat/cast -> slot value-string +
            // accesores; temporales se liberan tras comparar).
            struct OpRef {
                ir::IrValueId ptr = ir::IR_NO_VALUE;
                ir::IrValueId len = ir::IR_NO_VALUE;
                ir::IrValueId temp =
                    ir::IR_NO_VALUE; // !=NO_VALUE -> free si heap
            };
            auto op_ref = [&](ast::Expr *ex) -> OpRef {
                OpRef r;
                if (ex && ex->kind == ast::NodeKind::StringLitExpr &&
                    !static_cast<ast::StringLitExpr *>(ex)->is_interpolated()) {
                    auto *slit = static_cast<ast::StringLitExpr *>(ex);
                    const std::string &lit = slit->value;
                    std::vector<uint8_t> data(lit.begin(), lit.end());
                    data.push_back(0);
                    const uint64_t idx =
                        out_mod_->intern_static_data(std::move(data));
                    r.ptr = fn_->new_value(ir::IrType::PTR);
                    fn_->values[r.ptr].is_host_ptr = true;
                    ir::IrInstr sa{};
                    sa.op = ir::IrOp::STR_LIT_ADDR;
                    sa.type = ir::IrType::PTR;
                    sa.dst = r.ptr;
                    sa.imm = idx;
                    sa.source_line = e->loc.line;
                    emit(current_block_, std::move(sa));
                    r.len = emit_const(ir::IrType::I64,
                                       static_cast<uint64_t>(lit.size()),
                                       e->loc.line);
                    return r;
                }
                bool is_temp = false;
                if (ex && ex->kind == ast::NodeKind::BinaryExpr &&
                    static_cast<ast::BinaryExpr *>(ex)->op == ast::BinOp::Add &&
                    ex->result_type.kind == PrimitiveKind::STRING)
                    is_temp = true;
                else if (ex && ex->kind == ast::NodeKind::CastExpr &&
                         ex->result_type.kind == PrimitiveKind::STRING)
                    is_temp = true;
                ir::IrValueId v_slot = lower_expr(ex);
                if (v_slot == ir::IR_NO_VALUE) return r;
                r.ptr = emit_native_str_data_ptr(v_slot, e->loc.line);
                r.len = emit_native_str_len(v_slot, e->loc.line);
                if (is_temp) r.temp = v_slot;
                return r;
            };
            OpRef ra = op_ref(e->args[0].get());
            OpRef rb = op_ref(e->args[1].get());
            if (ra.ptr == ir::IR_NO_VALUE || rb.ptr == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId v_cmp = emit_strcmp_dispatched(ra.ptr, ra.len, rb.ptr,
                                                         rb.len, e->loc.line);
            if (ra.temp != ir::IR_NO_VALUE)
                emit_native_str_free_if_heap(ra.temp, e->loc.line);
            if (rb.temp != ir::IR_NO_VALUE)
                emit_native_str_free_if_heap(rb.temp, e->loc.line);
            // str_equals: bool = (strcmp == 0).
            ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, e->loc.line);
            ir::IrValueId v_eq = fn_->new_value(ir::IrType::BOOL);
            ir::IrInstr cmp{};
            cmp.op = ir::IrOp::CMP_EQ;
            cmp.type = ir::IrType::BOOL;
            cmp.dst = v_eq;
            cmp.operands = {v_cmp, v_zero};
            cmp.source_line = e->loc.line;
            emit(current_block_, std::move(cmp));
            out_value = v_eq;
            return true;
        }
        // Coerce string literals (PTR) a StringObject
        // (STRING handle) inline via STRMAKE.  Sin esto pasar un
        // literal directamente a str_concat/str_equals enviaria un
        // puntero raw como handle (UB).
        auto coerce_to_string_handle = [&](ast::Expr *ex) -> ir::IrValueId {
            if (ex && ex->kind == ast::NodeKind::StringLitExpr) {
                auto *sl = static_cast<ast::StringLitExpr *>(ex);
                // Tanto literales puros como interpolados.
                return lower_string_literal_to_string_object(sl);
            }
            return lower_expr(ex);
        };
        ir::IrValueId v_a = coerce_to_string_handle(e->args[0].get());
        ir::IrValueId v_b = coerce_to_string_handle(e->args[1].get());
        if (v_a == ir::IR_NO_VALUE || v_b == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // Sprint 6.D: usar STRCAT/STRCMP IR ops directos.  El helper
        // emit_strcat ya emite IR op puro con is_call_site=true.
        ir::IrValueId v_dst;
        if (is_str_concat) {
            v_dst = emit_strcat(v_a, v_b, e->loc.line);
        } else {
            v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr cmp{};
            cmp.op = ir::IrOp::STRCMP;
            cmp.type = ir::IrType::I64;
            cmp.dst = v_dst;
            cmp.operands = {v_a, v_b};
            cmp.source_line = e->loc.line;
            emit(current_block_, std::move(cmp));
        }
        // str_equals returns -1/0/1 (strcmp).  Convertir a bool: 0 == equal.
        if (is_str_equals) {
            ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, e->loc.line);
            ir::IrValueId v_eq = fn_->new_value(ir::IrType::BOOL);
            ir::IrInstr cmp{};
            cmp.op = ir::IrOp::CMP_EQ;
            cmp.type = ir::IrType::BOOL;
            cmp.dst = v_eq;
            cmp.operands = {v_dst, v_zero};
            cmp.source_line = e->loc.line;
            emit(current_block_, std::move(cmp));
            out_value = v_eq;
        } else {
            out_value = v_dst;
        }
        return true;
    }

    if (is_str_make) {
        if (e->args.size() != 2) {
            error_at(e->loc, "str_make: 2 args (ptr, len)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // Vesta Embed (native_poo_): str_make(ptr, len) COPIA len bytes a un
        // value-string PROPIO (sin GC), NO un StringObject GC.  Si len es un
        // literal entero -> Tier B (decision SSO/HEAP compile-time, sin rama).
        if (native_poo_) {
            int64_t known_len = -1;
            if (e->args[1] && e->args[1]->kind == ast::NodeKind::IntLitExpr) {
                int64_t lv =
                    (int64_t)static_cast<ast::IntLitExpr *>(e->args[1].get())
                        ->value;
                if (lv >= 0) known_len = lv;
            }
            ir::IrValueId v_ptr = lower_expr(e->args[0].get());
            ir::IrValueId v_len = lower_expr(e->args[1].get());
            if (v_ptr == ir::IR_NO_VALUE || v_len == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            out_value = build_native_string_from_buffer(v_ptr, v_len,
                                                        e->loc.line, known_len);
            return true;
        }
        ir::IrValueId v_ptr = lower_expr(e->args[0].get());
        ir::IrValueId v_len = lower_expr(e->args[1].get());
        if (v_ptr == ir::IR_NO_VALUE || v_len == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // Auto-detect: si el puntero proviene de memoria HOST (malloc,
        // str_cstr, gcallocp, etc.) emitimos `strmake_h` que lee bytes
        // del host.  Si es VM (subsp+&local, STR_LIT_ADDR, etc.) usamos
        // `strmake` original que lee de vm_mem.  Esto cierra el bug
        // historico en el que `str_make(buffer.data, len)` con `data`
        // mallocado retornaba zeros
        // Sprint 6.D: STRMAKE IR op.  El emitter elige strmake vs
        // strmake_h segun el flag is_host_ptr del SSA value v_ptr,
        // lo que reemplaza el if-else explicito anterior.
        out_value = emit_strmake(v_ptr, v_len, e->loc.line);
        return true;
    }

    // str_convert(s, enc) -> nuevo string con encoding
    // seleccionado.  El opcode strconv requiere encoding como inmediato
    // en el bytecode (no via registro), asi que el segundo arg debe
    // ser una constante numerica resuelta en compile time (literal int
    // o constante ENC_*).  Si no lo es, error claro.
    if (is_str_convert) {
        // Modelo de cadenas: un `string` es SIEMPRE una secuencia de code
        // points (UTF-8 por dentro), sin etiqueta de codificacion.  "Una
        // cadena en UTF-16" no es un valor del lenguaje, asi que convertir de
        // `string` a `string` no significa nada.
        //
        // La codificacion vive en la FRONTERA con codigo nativo: se pide el
        // buffer en la codificacion que espera esa API.  Ademas asi el mismo
        // codigo se comporta igual en interprete, JIT y AOT -- antes AOT
        // trataba la codificacion como advisory y divergia en silencio.
        error_at(e->loc,
                 "str_convert no existe: un `string` es siempre una secuencia "
                 "de code points.  La codificacion se elige al cruzar a codigo "
                 "nativo: usa `s.cstr()` para UTF-8 o `s.wstr()` para UTF-16");
        out_value = ir::IR_NO_VALUE;
        return true;
    }

    // ----- forName(string_lit) -----
    // Reflexion: devuelve ClassInfo* (i64 opaco) registrado en el
    // ClassRegistry por nombre.  Acepta SOLO un string literal.
    // Internamos el nombre en static_data (deduplicado:
    // si la clase ya esta declarada en __module_init, comparten idx).

    // ----- wait(obj) / notify(obj) / notifyAll(obj) -----
    // El argumento es CLASS (host pointer); las instrucciones monwait/
    // monnoti/monnota requieren GcHandle.  Convertimos primero via
    // gchandle (O(1) en el GcHeap) y luego ejecutamos la operacion.
    // Devuelven void; no participan en expresiones.
    if (is_wait || is_notify || is_notifyAll) {
        if (e->args.size() != 1) {
            error_at(e->loc, name + ": requiere exactamente 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_obj = lower_expr(e->args[0].get());
        if (v_obj == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // raw_asm-elim 2026-05-28: reemplazado el blob RAW_ASM original
        // por una secuencia de IR ops nativos (GC_HANDLE_FOR_PTR +
        // MONWAIT/MONNOTI/MONNOTA + MONENTER opcional).  Beneficios:
        //   (a) DCE puede eliminar el handle si la op se elimina.
        //   (b) El Selector JIT no tiene que parsear texto raw_asm.
        //   (c) Cada paso es individualmente reorderable por el optimizer.
        //   (d) Cero overhead vs el RAW_ASM previo: mismo bytecode emitido.
        //
        // BugFix t13 preservado: wait(obj) re-adquiere el monitor tras
        // despertar (semantica Java/POSIX condvar) via MONENTER explicito.
        const ir::IrValueId v_handle =
            emit_gc_handle_for_ptr(v_obj, e->loc.line);
        ir::IrOp mop = is_wait     ? ir::IrOp::MONWAIT
                       : is_notify ? ir::IrOp::MONNOTI
                                   : ir::IrOp::MONNOTA;
        {
            ir::IrInstr mi{};
            mi.op = mop;
            mi.type = ir::IrType::VOID;
            mi.dst = ir::IR_NO_VALUE;
            mi.operands = {v_handle};
            mi.source_line = e->loc.line;
            emit(current_block_, std::move(mi));
        }
        if (is_wait) {
            // Re-adquirir el monitor tras wake.
            ir::IrInstr me{};
            me.op = ir::IrOp::MONENTER;
            me.type = ir::IrType::VOID;
            me.dst = ir::IR_NO_VALUE;
            me.operands = {v_handle};
            me.source_line = e->loc.line;
            emit(current_block_, std::move(me));
        }
        out_value = ir::IR_NO_VALUE;
        return true;
    }

    // =====================================================================
    // Smart pointers builtins: unique<T> / shared<T>.
    // =====================================================================
    //
    // Modelo de slot: una variable @c unique<T> p esta bound a un SSA
    // value que es la DIRECCION de un slot stack de 8 bytes que
    // contiene el host_ptr al recurso.  Todas las operaciones acceden
    // al recurso via ese slot:
    //   get(p)            -> LOAD [slot]
    //   move(p) -> q      -> mvtake [q_slot], [p_slot]  (1 instr VM)
    //   cleanup scope exit -> LOAD ptr; CMP_EQ 0; CALL free(ptr) si no-null
    //
    // Para shared<T> el slot contiene un host_ptr al control block
    // gestionado por GC.  El control block tiene refcount@0, deleter@8,
    // payload inline desde +16.

    // ----- bug6 gc_box(value) -----  gc<T> para T CUALQUIERA.
    // Aloja el valor en un bloque GC-managed (GC_ALLOCP en interp/JIT,
    // vx_gc_alloc_ptr en AOT) de sizeof(T) bytes y devuelve el host_ptr al
    // box.  El GC recolecta el box cuando deja de ser alcanzable (stackmaps
    // precisos); no hay RAII (mismo modelo que gc<Clase>).  El valor interno
    // se lee con `*g` (deref).  Generaliza el modelo gc<Clase> (que aloja una
    // INSTANCIA de clase) a primitivos (gc<i64>), smart pointers (gc<unique<
    // i64>>) y anidamiento arbitrario (gc<shared<unique<i64>>>).
    if (is_gc_box) {
        if (e->args.size() != 1) {
            error_at(e->loc, "gc_box: requiere 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_payload = lower_expr(e->args[0].get());
        if (v_payload == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrType payload_t = fn_->values[v_payload].type;
        const Type sem_payload = e->args[0]->result_type;
        // Clasificacion identica a unique_box/shared_box: un struct value o
        // un smart-pointer wrapper tienen su "valor" en un BUFFER apuntado por
        // v_payload (payload_t==PTR al slot) -> se copia qword-a-qword.  Un
        // primitivo / raw-ptr / cfn es un VALOR de sizeof bytes -> STORE
        // directo.  Una CLASS es un host_ptr a objeto -> STORE directo del ptr.
        const bool payload_is_struct_value =
            (sem_payload.kind == PrimitiveKind::STRUCT) &&
            (tc_.struct_layouts().find(sem_payload.struct_name) !=
             tc_.struct_layouts().end());
        const bool payload_is_smart_wrapper =
            (sem_payload.kind == PrimitiveKind::UNIQUE_PTR ||
             sem_payload.kind == PrimitiveKind::SHARED_PTR ||
             sem_payload.kind == PrimitiveKind::BORROW ||
             sem_payload.kind == PrimitiveKind::BORROW_MUT);
        // sizeof(T) del contenido del box.
        uint64_t box_size = ir::type_access_bytes(payload_t);
        if (payload_is_struct_value) {
            box_size = static_cast<uint64_t>(
                tc_.struct_layouts().at(sem_payload.struct_name).size_bytes);
        } else if (payload_is_smart_wrapper) {
            box_size =
                (sem_payload.kind == PrimitiveKind::UNIQUE_PTR) ? 16u : 8u;
        }
        if (box_size == 0) box_size = 8u; // defensivo: nunca alocar 0 bytes.
        // %box = GC_ALLOCP(sizeof(T))  (host_ptr GC-managed).
        const ir::IrValueId v_box = emit_gc_allocp(
            emit_const(ir::IrType::I64, static_cast<int64_t>(box_size),
                       e->loc.line),
            e->loc.line);
        fn_->values[v_box].is_gc_object = true;
        fn_->values[v_box].is_host_ptr = true;
        if (payload_is_struct_value || payload_is_smart_wrapper) {
            // El payload es un PTR a un buffer (slot del wrapper / struct
            // value): copiar qword-a-qword al box.  Mismo mecanismo que
            // unique_box con struct/smart-wrapper.
            const uint64_t qwords = (box_size + 7) / 8;
            for (uint64_t i = 0; i < qwords; ++i) {
                const ir::IrValueId v_off = emit_const(
                    ir::IrType::I64, static_cast<int64_t>(i * 8), e->loc.line);
                const ir::IrValueId v_src_p = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_src_p].is_host_ptr =
                    fn_->values[v_payload].is_host_ptr;
                {
                    ir::IrInstr ad{};
                    ad.op = ir::IrOp::ADD;
                    ad.type = ir::IrType::I64;
                    ad.dst = v_src_p;
                    ad.operands = {v_payload, v_off};
                    ad.source_line = e->loc.line;
                    emit(current_block_, std::move(ad));
                }
                const ir::IrValueId v_word = fn_->new_value(ir::IrType::I64);
                {
                    ir::IrInstr ld{};
                    ld.op = ir::IrOp::LOAD;
                    ld.type = ir::IrType::I64;
                    ld.dst = v_word;
                    ld.operands = {v_src_p};
                    ld.source_line = e->loc.line;
                    emit(current_block_, std::move(ld));
                }
                const ir::IrValueId v_dst_p = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_dst_p].is_host_ptr = true;
                {
                    ir::IrInstr ad{};
                    ad.op = ir::IrOp::ADD;
                    ad.type = ir::IrType::I64;
                    ad.dst = v_dst_p;
                    ad.operands = {v_box, v_off};
                    ad.source_line = e->loc.line;
                    emit(current_block_, std::move(ad));
                }
                {
                    ir::IrInstr st{};
                    st.op = ir::IrOp::STORE;
                    st.type = ir::IrType::I64;
                    st.operands = {v_word, v_dst_p};
                    st.source_line = e->loc.line;
                    emit(current_block_, std::move(st));
                }
            }
            // Refcount inc-on-copy: si el payload es un shared<T> que viene de
            // COPIAR otra variable shared (IdentExpr, no shared_box/move), el
            // box es un DUEnO adicional del control block -> incrementar el
            // refcount.  Su SHAREDPTR_REL al exit lo decrementa (balance).  Un
            // shared_box(...) / move recien construido tiene refcount=1 y su
            // ownership se transfiere al box sin inc (mismo criterio que el
            // var-decl `shared<T> b = a` vs `shared<T> b = shared_box(...)`).
            if (sem_payload.kind == PrimitiveKind::SHARED_PTR &&
                e->args[0]->kind == ast::NodeKind::IdentExpr) {
                emit_shared_refcount_inc(v_box, e->loc.line);
            }
            // FINALIZADOR GC (cero fuga en escape): el box POSEE un recurso
            // interno (el deleter del unique / el control block del shared).
            // Si el box escapa su scope, el cleanup determinista no corre ->
            // registramos un finalizador GC que ejecuta EXACTAMENTE el mismo
            // deleter/dtor (resuelto por el contenido del propio box) cuando el
            // sweep colecte el box.  El caso no-escape lo desregistra en su
            // cleanup de scope (anti-doble-free).  Cero coste para
            // gc<primitivo> (no es smart-wrapper -> no lleva finalizador).
            if (sem_payload.kind == PrimitiveKind::UNIQUE_PTR) {
                emit_gc_set_finalizer(v_box, /*UNIQUE*/ 1, e->loc.line);
            } else if (sem_payload.kind == PrimitiveKind::SHARED_PTR) {
                emit_gc_set_finalizer(v_box, /*SHARED*/ 2, e->loc.line);
            }
        } else {
            // Primitivo / raw-ptr / cfn / CLASS: STORE directo del VALOR al
            // box.
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = payload_t;
            st.operands = {v_payload, v_box};
            st.source_line = e->loc.line;
            emit(current_block_, std::move(st));
        }
        out_value = v_box;
        return true;
    }

    // ----- unique_box(value) -----  unique<T> Tier 0 con deleter=free.
    // Layout: ALLOCA 8 bytes (slot) + malloc(sizeof(T)) (host) +
    // STORE value en host + STORE host_ptr en slot.  Cleanup al exit
    // del scope: LOAD slot; CMP_EQ 0; CALL free(ptr) si no-null.
    if (is_unique_box || is_shared_box) {
        if (e->args.size() != 1) {
            error_at(e->loc, name + ": requiere 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // Construccion IN-PLACE para `unique<Punto> p = {.x=10, .y=20}`:
        // si el arg es un InitListExpr con target_type_name anotado
        // (desugar de Opcion B en check_var_decl), alocamos host heap
        // PRIMERO y escribimos los campos DIRECTO sobre el host_ptr.
        // Cero memcpy stack -> heap.  Coste = solo los STOREs del init
        // list, igual que un struct value-type normal.
        //
        // El path generico mas abajo (lower_expr del arg + memcpy)
        // sigue cubriendo `unique_box(struct_var_existente)` y otros
        // casos donde el arg ya es un PTR a struct construido.
        if (is_unique_box && e->args[0]->kind == ast::NodeKind::InitListExpr) {
            auto *il = static_cast<ast::InitListExpr *>(e->args[0].get());
            if (!il->target_type_name.empty()) {
                const auto &layouts = tc_.struct_layouts();
                auto it_lay = layouts.find(il->target_type_name);
                if (it_lay != layouts.end()) {
                    const StructLayout &lay = it_lay->second;
                    // 1. Slot del unique<T> Tier 1 (16 bytes).  M7:
                    //    si lower_return seteo unique_box_target_slot_,
                    //    construimos directo en el retbuf del caller
                    //    (saltamos el stack_alloc_buf intermedio).
                    const ir::IrValueId v_slot =
                        (unique_box_target_slot_ != ir::IR_NO_VALUE)
                            ? unique_box_target_slot_
                            : stack_alloc_buf(16, e->loc.line);
                    // 2. RAW_ALLOC(sizeof_struct) -> host_ptr.
                    const ir::IrValueId v_size = emit_const(
                        ir::IrType::I64, static_cast<int64_t>(lay.size_bytes),
                        e->loc.line);
                    const ir::IrValueId v_host =
                        fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_host].is_host_ptr = true;
                    {
                        ir::IrInstr ins{};
                        ins.op = ir::IrOp::RAW_ALLOC;
                        ins.type = ir::IrType::PTR;
                        ins.dst = v_host;
                        ins.operands = {v_size};
                        ins.source_line = e->loc.line;
                        emit(current_block_, std::move(ins));
                    }
                    // 3. STOREs por campo directo al host_ptr.  Soporta
                    //    designated (.x=10) y posicional ({10, 20}).
                    for (size_t i = 0; i < il->elements.size(); ++i) {
                        // Encontrar el StructFieldInfo correspondiente.
                        const StructFieldInfo *fld = nullptr;
                        if (il->is_designated && i < il->field_names.size()) {
                            const std::string &fname = il->field_names[i];
                            for (const auto &f : lay.fields) {
                                if (f.name == fname) {
                                    fld = &f;
                                    break;
                                }
                            }
                            if (!fld) {
                                error_at(il->elements[i]->loc,
                                         "init list: campo '" + fname +
                                             "' no existe en struct '" +
                                             il->target_type_name + "'");
                                continue;
                            }
                        } else {
                            if (i >= lay.fields.size()) {
                                error_at(il->elements[i]->loc,
                                         "init list: demasiados elementos para "
                                         "struct '" +
                                             il->target_type_name + "'");
                                continue;
                            }
                            fld = &lay.fields[i];
                        }
                        // Lower el valor.
                        const ir::IrValueId v_val =
                            lower_expr(il->elements[i].get());
                        if (v_val == ir::IR_NO_VALUE) continue;
                        const ir::IrType ft =
                            ir_type_from_primitive(fld->type.kind);
                        // Cast si hace falta (literal int -> i32 del field,
                        // etc.).
                        const ir::IrType vt_from = fn_->values[v_val].type;
                        const ir::IrValueId v_casted = cast_if_needed(
                            v_val, vt_from, ft, il->elements[i]->loc.line,
                            /*is_explicit=*/true);
                        // Calcular addr destino = v_host + fld->offset.
                        ir::IrValueId v_dst = v_host;
                        if (fld->offset > 0) {
                            const ir::IrValueId v_off = emit_const(
                                ir::IrType::I64,
                                static_cast<int64_t>(fld->offset), e->loc.line);
                            const ir::IrValueId v_addr =
                                fn_->new_value(ir::IrType::PTR);
                            fn_->values[v_addr].is_host_ptr = true;
                            ir::IrInstr ad{};
                            ad.op = ir::IrOp::ADD;
                            ad.type = ir::IrType::I64;
                            ad.dst = v_addr;
                            ad.operands = {v_host, v_off};
                            ad.source_line = e->loc.line;
                            emit(current_block_, std::move(ad));
                            v_dst = v_addr;
                        }
                        // STORE val at [v_dst].
                        ir::IrInstr st{};
                        st.op = ir::IrOp::STORE;
                        st.type = ft;
                        st.operands = {v_casted, v_dst};
                        st.source_line = il->elements[i]->loc.line;
                        emit(current_block_, std::move(st));
                    }
                    // 4. STORE host_ptr al slot+0 del unique<T>.
                    {
                        ir::IrInstr st{};
                        st.op = ir::IrOp::STORE;
                        st.type = ir::IrType::I64;
                        st.operands = {v_host, v_slot};
                        st.source_line = e->loc.line;
                        emit(current_block_, std::move(st));
                    }
                    // 5. STORE deleter=0 (sentinel RAW_FREE) al slot+8.
                    {
                        const ir::IrValueId v_eight =
                            emit_const(ir::IrType::I64, 8, e->loc.line);
                        const ir::IrValueId v_slot8 =
                            fn_->new_value(ir::IrType::PTR);
                        ir::IrInstr ad{};
                        ad.op = ir::IrOp::ADD;
                        ad.type = ir::IrType::I64;
                        ad.dst = v_slot8;
                        ad.operands = {v_slot, v_eight};
                        ad.source_line = e->loc.line;
                        emit(current_block_, std::move(ad));
                        const ir::IrValueId v_zero =
                            emit_const(ir::IrType::I64, 0, e->loc.line);
                        ir::IrInstr st{};
                        st.op = ir::IrOp::STORE;
                        st.type = ir::IrType::I64;
                        st.operands = {v_zero, v_slot8};
                        st.source_line = e->loc.line;
                        emit(current_block_, std::move(st));
                    }
                    out_value = v_slot;
                    return true;
                }
            }
        }
        const ir::IrValueId v_payload = lower_expr(e->args[0].get());
        if (v_payload == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrType payload_t = fn_->values[v_payload].type;
        // Determinar el tipo Vesta semantico para saber si es struct value
        // (necesita memcpy a host heap) o si es CLASS/primitivo.
        const Type sem_payload = e->args[0]->result_type;
        const bool payload_is_struct_value =
            (sem_payload.kind == PrimitiveKind::STRUCT) &&
            (tc_.struct_layouts().find(sem_payload.struct_name) !=
             tc_.struct_layouts().end());
        // Un cfn (puntero a funcion crudo) es un VALOR de 8 bytes, igual que
        // un i64 -- NO un host_ptr a un objeto.  Aunque su IR type sea PTR,
        // debe alojar una celda heap de 8 bytes y guardar la direccion ahi
        // (como un primitivo), para que `ptr_of(p)` devuelva un cfn* y
        // `*ptr_of(p)` recupere el cfn con un solo LOAD.  Sin esto tomaria
        // la rama class-PTR (store directo) y `*ptr_of` deref-earia el codigo
        // de la funcion -> basura/#UD.
        const bool payload_is_cfn =
            (sem_payload.kind == PrimitiveKind::FUNCTION);
        // bug1: el payload es OTRO smart pointer (unique<T>/shared<T>/borrow).
        // Su @c lower_expr devuelve la DIRECCION del slot (igual que un struct
        // inline: su valor ES su buffer), con @c payload_t == PTR.  Si lo
        // trataramos como un host_ptr-a-objeto (store directo + free), el
        // cleanup haria RAW_FREE sobre la direccion del slot fuente (una ALLOCA
        // en vm_mem) -> SIGSEGV en VM/JIT.  La semantica correcta: el wrapper
        // externo POSEE una COPIA en heap del wrapper interno (sus bytes de
        // slot: shared/borrow=8, unique Tier 1=16).  Copiamos qword-a-qword
        // (mismo mecanismo que un struct value-type) para que el cleanup
        // RAW_FREE libere la copia heap -- NO el control block interno, que el
        // dueno interno (la variable `s`/`a`, aun en scope) decrementa/libera
        // por su cuenta.  Asi no hay double-free ni free de stack address.
        const bool payload_is_smart_wrapper =
            (sem_payload.kind == PrimitiveKind::UNIQUE_PTR ||
             sem_payload.kind == PrimitiveKind::SHARED_PTR ||
             sem_payload.kind == PrimitiveKind::BORROW ||
             sem_payload.kind == PrimitiveKind::BORROW_MUT);
        // bug2: el payload es un PUNTERO RAW (`i64*`, `void*`, etc.) -- un
        // VALOR de 8 bytes, NO un host_ptr a un objeto gestionado (a diferencia
        // de `new Class()` cuyo sem kind es CLASS).  Debe alojarse en una celda
        // heap de 8 bytes y guardar el puntero ahi (igual que cfn / primitivo),
        // para que `ptr_of(u)` devuelva `T**` (la celda) y `*ptr_of(u)`
        // recupere el `T*` con un solo LOAD.  Sin esto se tomaba la rama
        // store-directo (pensada para objetos CLASS) y `*ptr_of` deref-eaba el
        // VALOR del puntero como si fuera una direccion de slot -> se leia el
        // i64 apuntado y luego se deref-eaba ESE como direccion -> SIGSEGV.
        const bool payload_is_raw_ptr =
            (sem_payload.kind == PrimitiveKind::PTR);
        // sizeof(T): para primitivos usar ir_type_size; para structs
        // value-type consultar struct_layouts; para PTR/CLASS no se usa
        // (no alocamos memoria extra, guardamos el host_ptr directo).
        uint64_t payload_size = ir::type_access_bytes(payload_t);
        if (payload_is_struct_value) {
            payload_size = static_cast<uint64_t>(
                tc_.struct_layouts().at(sem_payload.struct_name).size_bytes);
        } else if (payload_is_smart_wrapper) {
            // Tamano del slot del wrapper interno: unique = 16 (Tier 1,
            // [ptr][deleter]), shared/borrow = 8 (un solo host_ptr/ctrl).
            payload_size =
                (sem_payload.kind == PrimitiveKind::UNIQUE_PTR) ? 16u : 8u;
        }
        if (is_unique_box) {
            // unique<T> Tier 1 (16 bytes):
            //   [+0 i64 ptr][+8 i64 deleter_addr]
            // deleter_addr = 0 (sentinel) -> cleanup hace RAW_FREE.
            // Layout 16 bytes para que el deleter info sobreviva
            // cuando la funcion devuelve el unique<T> via SRET.
            // M7: si lower_return seteo target slot, usar el retbuf del
            // caller directamente (skip allocacion intermedia en stack).
            const ir::IrValueId v_slot =
                (unique_box_target_slot_ != ir::IR_NO_VALUE)
                    ? unique_box_target_slot_
                    : unique_slot_buf(e->loc.line);

            // Bug fix bug2: si el payload ya es un host_ptr (e.g.
            // `new Recurso(1)` devuelve PTR), guardarlo DIRECTAMENTE
            // en slot[+0] sin doble indireccion via RAW_ALLOC.  De
            // lo contrario, el cleanup CALLVIRT al destructor opera
            // sobre el malloc'd region (8 bytes basura) en vez del
            // objeto Recurso real, y el dtor nunca se invoca.
            //
            // Para primitivos (i32, f64, etc.) seguimos usando
            // RAW_ALLOC porque `ptr_of(p)` debe devolver `T*` (host
            // memory).  Para PTR el `ptr_of` devuelve el mismo
            // host_ptr almacenado.
            ir::IrValueId v_to_store = v_payload;
            if (payload_is_struct_value || payload_is_smart_wrapper) {
                // Struct value-type O smart-pointer wrapper (bug1):
                // RAW_ALLOC(N) + memcpy qword-by-qword desde v_payload (PTR al
                // slot fuente) hacia v_payload_ptr (host heap).  Asi el
                // unique<T> ES dueno exclusivo de una copia en heap; el slot
                // fuente original puede morir al exit del scope sin afectar la
                // copia.  Para un wrapper interno, la copia contiene el
                // host_ptr/ctrl del wrapper (que su dueno original libera); el
                // cleanup del externo solo RAW_FREE-a esta celda de N bytes.
                const ir::IrValueId v_size =
                    emit_const(ir::IrType::I64,
                               static_cast<int64_t>(payload_size), e->loc.line);
                const ir::IrValueId v_payload_ptr =
                    fn_->new_value(ir::IrType::PTR);
                fn_->values[v_payload_ptr].is_host_ptr = true;
                {
                    ir::IrInstr ins{};
                    ins.op = ir::IrOp::RAW_ALLOC;
                    ins.type = ir::IrType::PTR;
                    ins.dst = v_payload_ptr;
                    ins.operands = {v_size};
                    ins.source_line = e->loc.line;
                    emit(current_block_, std::move(ins));
                }
                // Copy qword-by-qword (size redondeado hacia arriba a
                // multiplos de 8 bytes; el ultimo qword puede tener
                // padding pero no afecta correctness porque escribimos
                // sobre RAW_ALLOC zero-init y leemos desde el slot
                // ALLOCA que tiene tamano >= size_bytes).
                const uint64_t qwords = (payload_size + 7) / 8;
                for (uint64_t i = 0; i < qwords; ++i) {
                    const ir::IrValueId v_off =
                        emit_const(ir::IrType::I64, static_cast<int64_t>(i * 8),
                                   e->loc.line);
                    const ir::IrValueId v_src_p =
                        fn_->new_value(ir::IrType::PTR);
                    {
                        ir::IrInstr ad{};
                        ad.op = ir::IrOp::ADD;
                        ad.type = ir::IrType::I64;
                        ad.dst = v_src_p;
                        ad.operands = {v_payload, v_off};
                        ad.source_line = e->loc.line;
                        emit(current_block_, std::move(ad));
                    }
                    const ir::IrValueId v_word =
                        fn_->new_value(ir::IrType::I64);
                    {
                        ir::IrInstr ld{};
                        ld.op = ir::IrOp::LOAD;
                        ld.type = ir::IrType::I64;
                        ld.dst = v_word;
                        ld.operands = {v_src_p};
                        ld.source_line = e->loc.line;
                        emit(current_block_, std::move(ld));
                    }
                    const ir::IrValueId v_dst_p =
                        fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_dst_p].is_host_ptr = true;
                    {
                        ir::IrInstr ad{};
                        ad.op = ir::IrOp::ADD;
                        ad.type = ir::IrType::I64;
                        ad.dst = v_dst_p;
                        ad.operands = {v_payload_ptr, v_off};
                        ad.source_line = e->loc.line;
                        emit(current_block_, std::move(ad));
                    }
                    {
                        ir::IrInstr st{};
                        st.op = ir::IrOp::STORE;
                        st.type = ir::IrType::I64;
                        st.operands = {v_word, v_dst_p};
                        st.source_line = e->loc.line;
                        emit(current_block_, std::move(st));
                    }
                }
                v_to_store = v_payload_ptr;
            } else if (payload_t != ir::IrType::PTR || payload_is_cfn ||
                       payload_is_raw_ptr) {
                // RAW_ALLOC(payload_size) -> v_payload_ptr (host ptr).
                // (cfn y punteros raw entran aqui pese a ser PTR: son valores
                //  de 8 bytes que se cajean, no host_ptrs a objetos.)
                const ir::IrValueId v_size =
                    emit_const(ir::IrType::I64,
                               static_cast<int64_t>(payload_size), e->loc.line);
                const ir::IrValueId v_payload_ptr =
                    fn_->new_value(ir::IrType::PTR);
                fn_->values[v_payload_ptr].is_host_ptr = true;
                {
                    ir::IrInstr ins{};
                    ins.op = ir::IrOp::RAW_ALLOC;
                    ins.type = ir::IrType::PTR;
                    ins.dst = v_payload_ptr;
                    ins.operands = {v_size};
                    ins.source_line = e->loc.line;
                    emit(current_block_, std::move(ins));
                }
                // STORE payload at [v_payload_ptr] (host memory).
                {
                    ir::IrInstr st{};
                    st.op = ir::IrOp::STORE;
                    st.type = payload_t;
                    st.operands = {v_payload, v_payload_ptr};
                    st.source_line = e->loc.line;
                    emit(current_block_, std::move(st));
                }
                v_to_store = v_payload_ptr;
            }
            // STORE v_to_store at [v_slot+0].
            //   Para primitivos: v_to_store = malloc'd ptr -> RAW_FREE valido.
            //   Para PTR (class/struct): v_to_store = host_ptr al objeto;
            //                            cleanup hace CALLVIRT dtor + skip
            //                            free.
            {
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::I64;
                st.operands = {v_to_store, v_slot};
                st.source_line = e->loc.line;
                emit(current_block_, std::move(st));
            }
            // STORE deleter=0 at [v_slot+8] (sentinel = RAW_FREE).
            {
                const ir::IrValueId v_eight =
                    emit_const(ir::IrType::I64, 8, e->loc.line);
                const ir::IrValueId v_slot8 = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr add{};
                add.op = ir::IrOp::ADD;
                add.type = ir::IrType::I64;
                add.dst = v_slot8;
                add.operands = {v_slot, v_eight};
                add.source_line = e->loc.line;
                emit(current_block_, std::move(add));
                const ir::IrValueId v_zero =
                    emit_const(ir::IrType::I64, 0, e->loc.line);
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::I64;
                st.operands = {v_zero, v_slot8};
                st.source_line = e->loc.line;
                emit(current_block_, std::move(st));
            }
            fn_->values[v_slot].pointee_is_host_ptr = true;
            out_value = v_slot;
            return true;
        } else {
            // shared<T> (H3 no-GC): RAW_ALLOC(16 + 8) del bloque de control.
            // Layout: [+0 i64 refcount=1][+8 u64 deleter=0][+16 T payload].
            // El slot stack guarda host_ptr al control block.  El cleanup
            // SHAREDPTR_REL hace `free` cuando el refcount cae a 0 (refcount
            // puro, determinista, sin GC -> funciona en AOT standalone).
            const ir::IrValueId v_slot = stack_alloc_buf(8, e->loc.line);
            const ir::IrValueId v_ctrl_size = emit_const(
                ir::IrType::I64, 16 + 8, e->loc.line); // 24 bytes total
            // RAW_ALLOC -> host_ptr al bloque de control.
            const ir::IrValueId v_ctrl = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_ctrl].is_host_ptr = true;
            {
                ir::IrInstr ins{};
                ins.op = ir::IrOp::RAW_ALLOC;
                ins.type = ir::IrType::PTR;
                ins.dst = v_ctrl;
                ins.operands = {v_ctrl_size};
                ins.source_line = e->loc.line;
                emit(current_block_, std::move(ins));
            }
            // STORE refcount=1 at [v_ctrl + 0].
            {
                const ir::IrValueId v_one =
                    emit_const(ir::IrType::I64, 1, e->loc.line);
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::I64;
                st.operands = {v_one, v_ctrl};
                st.source_line = e->loc.line;
                emit(current_block_, std::move(st));
            }
            // STORE deleter=0 at [v_ctrl + 8] (placeholder; cleanup usa free
            // literal).
            {
                const ir::IrValueId v_eight =
                    emit_const(ir::IrType::I64, 8, e->loc.line);
                const ir::IrValueId v_ctrl8 = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_ctrl8].is_host_ptr = true;
                ir::IrInstr add{};
                add.op = ir::IrOp::ADD;
                add.type = ir::IrType::I64;
                add.dst = v_ctrl8;
                add.operands = {v_ctrl, v_eight};
                add.source_line = e->loc.line;
                emit(current_block_, std::move(add));
                const ir::IrValueId v_zero =
                    emit_const(ir::IrType::I64, 0, e->loc.line);
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::I64;
                st.operands = {v_zero, v_ctrl8};
                st.source_line = e->loc.line;
                emit(current_block_, std::move(st));
            }
            // STORE payload at [v_ctrl + 16].
            {
                const ir::IrValueId v_sixteen =
                    emit_const(ir::IrType::I64, 16, e->loc.line);
                const ir::IrValueId v_ctrl16 = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_ctrl16].is_host_ptr = true;
                ir::IrInstr add{};
                add.op = ir::IrOp::ADD;
                add.type = ir::IrType::I64;
                add.dst = v_ctrl16;
                add.operands = {v_ctrl, v_sixteen};
                add.source_line = e->loc.line;
                emit(current_block_, std::move(add));
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = payload_t;
                st.operands = {v_payload, v_ctrl16};
                st.source_line = e->loc.line;
                emit(current_block_, std::move(st));
            }
            // STORE v_ctrl at [v_slot] (VM memory).
            {
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::I64;
                st.operands = {v_ctrl, v_slot};
                st.source_line = e->loc.line;
                emit(current_block_, std::move(st));
            }
            fn_->values[v_slot].pointee_is_host_ptr = true;
            out_value = v_slot;
            return true;
        }
    }

    // ----- unique_with(value, deleter_fn) / shared_with(...) -----
    // Forma generica donde el programador especifica el deleter.
    // No se hace alloc: el value es el RESULTADO de una alocacion ya
    // hecha (VirtualAlloc, malloc, fopen, socket(), etc.).  El
    // cleanup en scope exit invoca deleter_fn(value) automaticamente.
    //
    // Layout: ALLOCA 8 (slot) + STORE value at [slot].  Cleanup:
    // LOAD ptr; if (ptr != 0) CALL deleter(ptr); zero slot.
    //
    // El nombre del deleter se almacena en CleanupAction::literal_deleter
    // con prefijo "@extern:kVestaIoLib:fn" si es extern, o el nombre puro si
    // es Vesta.  El emit_cleanups_all elige CALLN o CALLVM.
    if (is_unique_with || is_shared_with) {
        if (e->args.size() != 2) {
            error_at(e->loc, name + ": requiere 2 argumentos");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_payload = lower_expr(e->args[0].get());
        if (v_payload == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // Validar que arg[1] sea IdentExpr (type_checker ya lo verifico).
        if (e->args[1]->kind != ast::NodeKind::IdentExpr) {
            error_at(e->args[1]->loc,
                     name +
                         ": el deleter debe ser un identificador de funcion");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const auto *deleter_id =
            static_cast<const ast::IdentExpr *>(e->args[1].get());
        // Capturamos el nombre del deleter; el cleanup lo usara.
        std::string deleter_label =
            tc_.lookup_extern_qualified(deleter_id->name);
        if (deleter_label.empty()) {
            // No es extern -> es funcion Vesta.  Usamos el nombre puro;
            // el cleanup emitira CALLVM @Absolute("code.<name>").
            deleter_label = deleter_id->name;
        } // else: ya viene con prefijo "@extern:kVestaIoLib:fn".
        // Tier 1: slot 16 + STORE value@+0 + STORE deleter_addr@+8.  HEAP si
        // el unique va a un campo (unique_slot_buf), si no STACK.
        const ir::IrValueId v_slot = unique_slot_buf(e->loc.line);
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.operands = {v_payload, v_slot};
            st.source_line = e->loc.line;
            emit(current_block_, std::move(st));
        }
        // STORE deleter address en slot+8.  Materializamos la
        // direccion via RAW_ASM: `mov {dst}, @Absolute("code.<fn>")`.
        // El assembler resuelve la direccion al linker time.
        //
        // Limitacion: para deleters extern no podemos obtener una
        // direccion vesta-callable, por lo que usamos 0 (sentinel)
        // y el cleanup local conoce el deleter por compile-time via
        // literal_deleter.  SRET return con extern deleter no
        // preserva la info (futuro: añadir tabla de deleter ids).
        const ir::IrValueId v_deleter_addr = fn_->new_value(ir::IrType::I64);
        if (deleter_label == "free" ||
            deleter_label.rfind("@extern:", 0) == 0) {
            // Deleter "free" (builtin, no una fn Vesta): NO hay `code.free`
            // que direccionar -> se almacena 0, el sentinel que el dtor del
            // slot (emit_free_unique_slot) interpreta como RAW_FREE (== free
            // null-safe).  Sin esto un unique_with(malloc(..), free) que va a
            // un CAMPO (SRET) emitiria `@Absolute("code.free")` -> el linker
            // no resuelve el simbolo (RelocationError code.free).
            // Extern (`@extern:kVestaIoLib:fn`): tampoco es direccionable como fn
            // Vesta; mismo sentinel 0 + literal_deleter local para el call.
            const ir::IrValueId v_zero =
                emit_const(ir::IrType::I64, 0, e->loc.line);
            ir::IrInstr mov{};
            mov.op = ir::IrOp::MOV;
            mov.type = ir::IrType::I64;
            mov.dst = v_deleter_addr;
            mov.operands = {v_zero};
            mov.source_line = e->loc.line;
            emit(current_block_, std::move(mov));
        } else {
            // Vesta: emitir LABEL_ADDR -> v_deleter_addr.
            ir::IrValueId v_label = emit_label_addr(deleter_label, e->loc.line);
            ir::IrInstr mov{};
            mov.op = ir::IrOp::MOV;
            mov.type = ir::IrType::I64;
            mov.dst = v_deleter_addr;
            mov.operands = {v_label};
            mov.source_line = e->loc.line;
            emit(current_block_, std::move(mov));
        }
        // STORE deleter_addr en slot+8.  v_slot8 HEREDA la host-ness de v_slot
        // (heap -> movh, stack -> mov) para que sea consistente con la store de
        // slot+0 y con las lecturas del dtor; sin esto el deleter se escribiria
        // en vm_mem con un slot heap -> el dtor leeria 0 -> RAW_FREE en vez de
        // invocar el deleter.
        {
            const ir::IrValueId v_eight =
                emit_const(ir::IrType::I64, 8, e->loc.line);
            const ir::IrValueId v_slot8 = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_slot8].is_host_ptr = fn_->values[v_slot].is_host_ptr;
            ir::IrInstr add{};
            add.op = ir::IrOp::ADD;
            add.type = ir::IrType::I64;
            add.dst = v_slot8;
            add.operands = {v_slot, v_eight};
            add.source_line = e->loc.line;
            emit(current_block_, std::move(add));
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.operands = {v_deleter_addr, v_slot8};
            st.source_line = e->loc.line;
            emit(current_block_, std::move(st));
        }
        // El slot contiene un valor con semantica de host_ptr / handle.
        fn_->values[v_slot].pointee_is_host_ptr = true;
        // Anotamos la accion de cleanup pendiente para que el cleanup
        // local pueda usar el deleter por compile-time (cero overhead).
        // El cleanup dinamico via slot+8 solo se activa cuando se
        // accede al smart pointer tras SRET (no tenemos info compile-time).
        pending_smartptr_deleter_ = deleter_label;
        out_value = v_slot;
        return true;
    }

    // ----- move(p) -----  transfer ownership.
    // Cuando `move(p)` es el init DIRECTO de un var-decl, lo maneja
    // lower_var_decl (emite mvtake al slot del var + zerifica el origen) y
    // NUNCA llega aqui.  Pero `move(p)` tambien aparece como ARGUMENTO de
    // funcion (`consume(move(data))`), en una asignacion o en un return.  En
    // esos casos DEBEMOS emitir el mvtake a un TEMPORAL aqui mismo, o el slot
    // origen NO se zerifica -> tanto el origen como el destino liberan el
    // MISMO box -> doble-free (en GC/interp es no-op, en native abort).
    //
    // Replicamos la logica del var-decl: ALLOCA temporal + mvtake [tmp+0]<-
    // [src+0] (+ [tmp+8]<-[src+8] para unique<T>), que MUEVE el contenido y
    // ZERIFICA el origen.  Devolvemos el temporal (el call lee tmp[0] = box).
    if (is_move) {
        if (e->args.size() != 1) {
            error_at(e->loc, "move: requiere 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_src = lower_expr(e->args[0].get());
        if (v_src == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // unique<T> Tier 1: slot = 16 bytes (ptr + deleter).
        // shared<T>: slot = 8 bytes (ctrl_block_ptr).
        const bool is_unique =
            (e->args[0]->result_type.kind == PrimitiveKind::UNIQUE_PTR);
        const uint32_t slot_bytes = is_unique ? 16 : 8;
        // bug3: si el resultado del move aterriza en un CAMPO owned
        // (unique_slot_to_heap_ set por lower_assign), el slot destino debe
        // vivir en HEAP (RAW_ALLOC) para sobrevivir al scope y ser liberado por
        // el dtor del contenedor.  En cualquier otro caso (arg de funcion,
        // var-decl local, return) sigue siendo un ALLOCA de stack.
        const bool move_to_heap = unique_slot_to_heap_;
        if (move_to_heap) unique_slot_to_heap_ = false;
        const ir::IrValueId v_tmp = fn_->new_value(ir::IrType::PTR);
        if (move_to_heap) {
            fn_->values[v_tmp].is_host_ptr = true;
            const ir::IrValueId v_size =
                emit_const(ir::IrType::I64, slot_bytes, e->loc.line);
            ir::IrInstr al{};
            al.op = ir::IrOp::RAW_ALLOC;
            al.type = ir::IrType::PTR;
            al.dst = v_tmp;
            al.operands = {v_size};
            al.source_line = e->loc.line;
            emit(current_block_, std::move(al));
        } else {
            ir::IrInstr al{};
            al.op = ir::IrOp::ALLOCA;
            al.type = ir::IrType::I8;
            al.dst = v_tmp;
            al.imm = slot_bytes;
            al.source_line = e->loc.line;
            emit(current_block_, std::move(al));
        }
        if (move_to_heap) {
            // bug3: destino en HEAP (host_ptr).  `mvtake` (opcode 0x72) opera
            // SIEMPRE sobre vm_mem, por lo que NO puede escribir un host_ptr en
            // VM/JIT.  Emitimos el move explicito con addressing host-aware:
            // LOAD [src+off] (vm_mem, el slot fuente es un ALLOCA local) ->
            // STORE [tmp+off] (movh, host); luego STORE 0 [src+off] para
            // invalidar el origen (evita double-free).  qword a qword.
            const uint32_t qwords = slot_bytes / 8;
            for (uint32_t i = 0; i < qwords; ++i) {
                const ir::IrValueId v_off = emit_const(
                    ir::IrType::I64, static_cast<int64_t>(i * 8), e->loc.line);
                // src_p = v_src + off  (VM addr).
                ir::IrValueId v_src_p = v_src;
                ir::IrValueId v_dst_p = v_tmp;
                if (i > 0) {
                    v_src_p = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr a1{};
                    a1.op = ir::IrOp::ADD;
                    a1.type = ir::IrType::I64;
                    a1.dst = v_src_p;
                    a1.operands = {v_src, v_off};
                    a1.source_line = e->loc.line;
                    emit(current_block_, std::move(a1));
                    v_dst_p = fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_dst_p].is_host_ptr = true;
                    ir::IrInstr a2{};
                    a2.op = ir::IrOp::ADD;
                    a2.type = ir::IrType::I64;
                    a2.dst = v_dst_p;
                    a2.operands = {v_tmp, v_off};
                    a2.source_line = e->loc.line;
                    emit(current_block_, std::move(a2));
                }
                // word = LOAD [src_p]  (vm_mem).
                const ir::IrValueId v_word = fn_->new_value(ir::IrType::I64);
                {
                    ir::IrInstr ld{};
                    ld.op = ir::IrOp::LOAD;
                    ld.type = ir::IrType::I64;
                    ld.dst = v_word;
                    ld.operands = {v_src_p};
                    ld.source_line = e->loc.line;
                    emit(current_block_, std::move(ld));
                }
                // STORE word -> [dst_p]  (host, movh por is_host_ptr).
                {
                    ir::IrInstr st{};
                    st.op = ir::IrOp::STORE;
                    st.type = ir::IrType::I64;
                    st.operands = {v_word, v_dst_p};
                    st.source_line = e->loc.line;
                    emit(current_block_, std::move(st));
                }
                // STORE 0 -> [src_p]  (zerifica origen, vm_mem).
                {
                    const ir::IrValueId v_zero =
                        emit_const(ir::IrType::I64, 0, e->loc.line);
                    ir::IrInstr st{};
                    st.op = ir::IrOp::STORE;
                    st.type = ir::IrType::I64;
                    st.operands = {v_zero, v_src_p};
                    st.source_line = e->loc.line;
                    emit(current_block_, std::move(st));
                }
            }
            fn_->values[v_tmp].pointee_is_host_ptr = true;
            out_value = v_tmp;
            return true;
        }
        // mvtake [tmp+0] <- [src+0]  (mueve el box-ptr + zerifica origen).
        emit_mvtake(v_tmp, v_src, e->loc.line);
        if (slot_bytes == 16) {
            // Segundo qword: deleter.
            const ir::IrValueId v_eight =
                emit_const(ir::IrType::I64, 8, e->loc.line);
            const ir::IrValueId v_tmp8 = fn_->new_value(ir::IrType::PTR);
            const ir::IrValueId v_src8 = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr add{};
                add.op = ir::IrOp::ADD;
                add.type = ir::IrType::I64;
                add.dst = v_tmp8;
                add.operands = {v_tmp, v_eight};
                add.source_line = e->loc.line;
                emit(current_block_, std::move(add));
            }
            {
                ir::IrInstr add{};
                add.op = ir::IrOp::ADD;
                add.type = ir::IrType::I64;
                add.dst = v_src8;
                add.operands = {v_src, v_eight};
                add.source_line = e->loc.line;
                emit(current_block_, std::move(add));
            }
            emit_mvtake(v_tmp8, v_src8, e->loc.line);
        }
        fn_->values[v_tmp].pointee_is_host_ptr = true;
        out_value = v_tmp;
        return true;
    }

    // ----- Z.6: is_shared(obj) -> bool -----
    // Pipeline IR puro:
    //   1. RAW_ASM gchandle dst,src  -> dst = uint32 handle.
    //   2. CONST u64 0x80000000.
    //   3. IrOp::AND_BIT handle & MASK.
    //   4. IrOp::SHR_U result, 31 -> resultado en bit 0 (0 o 1).
    //   5. cast a bool.

    // Z.10: shared_heap_live_count() / shared_heap_bytes() /
    // shared_gc_collect() Lowering comun: const op + sharedstat opcode.
    if (is_z10_live_count || is_z10_bytes || is_z10_gc_collect) {
        const int op_code = is_z10_live_count ? 0 : is_z10_bytes ? 1 : 2;
        const ir::IrType ret_type =
            is_z10_bytes
                ? ir::IrType::U64
                : (is_z10_live_count ? ir::IrType::U32 : ir::IrType::VOID);
        // raw_asm-elim wave 3: SHARED_STAT IR op dedicado.  El emit
        // del bytecode usa `r14` como dst dummy para el caso VOID
        // (op_code=2 gc_collect) automaticamente.
        const ir::IrValueId v_op =
            emit_const(ir::IrType::I32, (uint64_t)op_code, e->loc.line);
        const ir::IrValueId v_dst = (ret_type == ir::IrType::VOID)
                                        ? ir::IR_NO_VALUE
                                        : fn_->new_value(ret_type);
        ir::IrInstr ss{};
        ss.op = ir::IrOp::SHARED_STAT;
        ss.type = ret_type;
        ss.dst = v_dst;
        ss.operands = {v_op};
        ss.source_line = e->loc.line;
        emit(current_block_, std::move(ss));
        out_value = v_dst;
        return true;
    }

    // ----- ptr_of(p) -----  T* host, sin consumir el smart pointer.
    // unique<T>: LOAD ptr from [slot]; resultado is_host_ptr.
    // shared<T>: LOAD ctrl from [slot]; ADD 16; resultado is_host_ptr.
    if (is_get) {
        if (e->args.size() != 1) {
            error_at(e->loc, "ptr_of: requiere 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const Type arg_t = e->args[0]->result_type;
        const ir::IrValueId v_slot = lower_expr(e->args[0].get());
        if (v_slot == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // LOAD ptr from [v_slot].
        const ir::IrValueId v_ptr = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_ptr].is_host_ptr = true;
        // BugFix R2: si el inner T es CLASS, marcar is_gc_object=true
        // para que el regalloc trate al SSA value como handle GC y
        // preserve su naturaleza a traves de CALLVIRTs (el GC scan
        // pratico de A.34.fix8 lo encuentra como root via stack).
        const bool inner_is_class =
            arg_t.pointee && arg_t.pointee->kind == PrimitiveKind::CLASS;
        if (inner_is_class) {
            fn_->values[v_ptr].is_gc_object = true;
        }
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_ptr;
            ld.operands = {v_slot};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
        }
        if (arg_t.kind == PrimitiveKind::SHARED_PTR) {
            // shared<T>: payload esta en +16 del control block.
            const ir::IrValueId v_sixteen =
                emit_const(ir::IrType::I64, 16, e->loc.line);
            const ir::IrValueId v_pay = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_pay].is_host_ptr = true;
            ir::IrInstr add{};
            add.op = ir::IrOp::ADD;
            add.type = ir::IrType::I64;
            add.dst = v_pay;
            add.operands = {v_ptr, v_sixteen};
            add.source_line = e->loc.line;
            emit(current_block_, std::move(add));
            // BugFix R2: si inner es CLASS, el slot @+16 guarda el
            // host_ptr al objeto.  Hacer otro LOAD para obtenerlo y
            // marcarlo como is_gc_object para CALLVIRT.  Sin esto,
            // ptr_of(shared<Class>).method() recibia el addr del SLOT
            // (no del Class) -> CALLVIRT con this invalido.
            if (inner_is_class) {
                const ir::IrValueId v_obj = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_obj].is_host_ptr = true;
                fn_->values[v_obj].is_gc_object = true;
                ir::IrInstr ld2{};
                ld2.op = ir::IrOp::LOAD;
                ld2.type = ir::IrType::I64;
                ld2.dst = v_obj;
                ld2.operands = {v_pay};
                ld2.source_line = e->loc.line;
                emit(current_block_, std::move(ld2));
                out_value = v_obj;
                return true;
            }
            out_value = v_pay;
            return true;
        }
        // unique<T>: ptr ES el payload.
        out_value = v_ptr;
        return true;
    }

    // ----- use_count(s) -----  i64 refcount del shared<T>.
    if (is_use_count) {
        if (e->args.size() != 1) {
            error_at(e->loc, "use_count: requiere 1 argumento");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_slot = lower_expr(e->args[0].get());
        if (v_slot == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // LOAD ctrl from [slot].
        const ir::IrValueId v_ctrl = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_ctrl].is_host_ptr = true;
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_ctrl;
            ld.operands = {v_slot};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
        }
        // LOAD refcount from [ctrl + 0] (host memory).
        const ir::IrValueId v_rc = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_rc;
            ld.operands = {v_ctrl};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
        }
        out_value = v_rc;
        return true;
    }

    // =====================================================================
    // Borrow checker builtins: lend / lend_mut / read_borrow / write_borrow.
    // =====================================================================
    //
    // El borrow checker compile-time ya valido las reglas R1-R4.  El
    // lowering solo emite el codigo correspondiente con cero overhead
    // vs un raw pointer:
    //
    //   lend(owner)       -> ptr_of equivalente al unique<T>/shared<T>.
    //                        Para owner que NO es smart pointer (var
    //                        local plain), emite &owner via slot stack.
    //   lend_mut(owner)   -> mismo bytecode que lend; la distincion
    //                        es puramente compile-time.
    //   read_borrow(b)    -> LOAD a traves del host_ptr (movh).
    //   write_borrow(m,v) -> STORE a traves del host_ptr (movh).
    if (is_lend || is_lend_mut) {
        if (e->args.size() != 1) {
            error_at(e->loc, name + ": requiere 1 argumento (owner)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        /* El prestamo no deja rastro en el IR -- esto no emite instruccion, el
         * puntero es el mismo --, asi que lo que el borrow checker demuestra se
         * quedaba dentro del type checker.  Se apunta como HECHO del IR, con su
         * procedencia, para que el analisis pueda componerlo con regiones y
         * efectos.  Anotar no cambia el codigo generado. */
        auto anota_prestamo = [&](ir::IrValueId v_pres, ir::IrValueId v_owner) {
            if (v_pres == ir::IR_NO_VALUE || !fn_) return;
            ir::IrFunction::BorrowFact bf;
            bf.value = v_pres;
            bf.owner = v_owner;
            bf.mutable_ = is_lend_mut;
            /* La naturaleza de lo prestado viaja con el hecho: prestar un
             * `unique` no es lo mismo que prestar un local, y nadie debe
             * confundirlos despues. */
            const Type &ot = e->args[0]->result_type;
            using OK = ir::IrFunction::BorrowOwnerKind;
            bf.owner_kind = (ot.kind == PrimitiveKind::UNIQUE_PTR) ? OK::Unique
                            : (ot.kind == PrimitiveKind::SHARED_PTR)
                                ? OK::Shared
                            : (ot.kind == PrimitiveKind::BORROW ||
                               ot.kind == PrimitiveKind::BORROW_MUT)
                                ? OK::Reborrow
                                : OK::Plain;
            bf.line = e->loc.line;
            if (e->args[0]->kind == ast::NodeKind::IdentExpr)
                bf.owner_name =
                    static_cast<ast::IdentExpr *>(e->args[0].get())->name;
            fn_->borrow_facts.push_back(std::move(bf));
        };
        // Si el owner es unique<T>/shared<T>, equivale a ptr_of(owner)
        // que carga slot+0.  Si es una variable plain, devolvemos
        // &owner (su SSA value, que ya tiene is_host_ptr correcto
        // gracias al pre-pase de address-taken promotion: cualquier
        // local cuya direccion se toma con @c & se promociona a slot
        // estable en stack en lugar de vivir solo en un SSA value).
        const Type owner_t = e->args[0]->result_type;
        // F3 reborrow: si el arg ES un borrow/borrow_mut (var o param),
        // su SSA value YA ES el host_ptr al payload.  No queremos
        // emitir LOAD via read_local (eso es para slots de unique).
        // Bypass: usar lookup directamente cuando el arg es un
        // IdentExpr cuyo tipo es borrow.
        if (e->args[0]->kind == ast::NodeKind::IdentExpr &&
            (owner_t.kind == PrimitiveKind::BORROW ||
             owner_t.kind == PrimitiveKind::BORROW_MUT)) {
            auto *id = static_cast<ast::IdentExpr *>(e->args[0].get());
            const ir::IrValueId v = lookup(id->name);
            if (v != ir::IR_NO_VALUE) {
                // El borrow_var ya es host_ptr; lo devolvemos tal cual.
                // (read_borrow/write_borrow lo usaran con movh.)
                anota_prestamo(v, v); // represtamo: el owner ES otro prestamo
                out_value = v;
                return true;
            }
        }
        // Owner plain (i32, i64, etc.): si es un IdentExpr de variable
        // address-taken, el SSA value del scope es la DIRECCION del
        // ALLOCA (no el valor).  Hacer lower_expr pasaria por lower_ident
        // que para address-taken locals emite un LOAD i32 (devuelve el
        // VALOR), corrompiendo read_borrow/write_borrow posteriores.
        // Bypass: lookup directo cuando el arg es IdentExpr y la var
        // esta address-taken (lo cual el scan_address_taken garantiza
        // que sea verdad para `lend(local)` en owner plain).
        if (e->args[0]->kind == ast::NodeKind::IdentExpr) {
            auto *id = static_cast<ast::IdentExpr *>(e->args[0].get());
            if (address_taken_locals_.count(id->name)) {
                const ir::IrValueId v = lookup(id->name);
                if (v != ir::IR_NO_VALUE) {
                    // v es PTR al slot del local.  Marcamos is_host_ptr
                    // (es un slot vm-mem en stack, pero la convencion
                    // para borrows es host_ptr; read_borrow/write_borrow
                    // emiten LOAD/STORE indirecto con movh sobre este).
                    // En realidad el slot vive en vm_mem (ALLOCA),
                    // asi que NO marcamos is_host_ptr aqui: los LOAD/
                    // STORE de read/write_borrow ya consultan eso del
                    // SSA value y emiten mov (no movh) si es slot VM.
                    anota_prestamo(v, v);
                    out_value = v;
                    return true;
                }
            }
        }
        const ir::IrValueId v_arg = lower_expr(e->args[0].get());
        if (v_arg == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        if (owner_t.kind == PrimitiveKind::UNIQUE_PTR ||
            owner_t.kind == PrimitiveKind::SHARED_PTR) {
            // LOAD slot+0 (para unique) o ctrl+16 (shared payload).
            const ir::IrValueId v_ptr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_ptr].is_host_ptr = true;
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_ptr;
            ld.operands = {v_arg};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
            if (owner_t.kind == PrimitiveKind::SHARED_PTR) {
                // Para shared, sumar 16 (offset del payload inline en
                // ctrl_block: refcount@0 + deleter@8 + payload@16).
                const ir::IrValueId v_sixteen =
                    emit_const(ir::IrType::I64, 16, e->loc.line);
                const ir::IrValueId v_pay = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_pay].is_host_ptr = true;
                ir::IrInstr add{};
                add.op = ir::IrOp::ADD;
                add.type = ir::IrType::I64;
                add.dst = v_pay;
                add.operands = {v_ptr, v_sixteen};
                add.source_line = e->loc.line;
                emit(current_block_, std::move(add));
                anota_prestamo(v_pay, v_arg);
                out_value = v_pay;
                return true;
            }
            anota_prestamo(v_ptr, v_arg);
            out_value = v_ptr;
            return true;
        }
        // owner plain: el SSA value ya es la direccion (address-taken).
        // Lo devolvemos tal cual.
        anota_prestamo(v_arg, v_arg);
        out_value = v_arg;
        return true;
    }
    if (is_read_borrow) {
        if (e->args.size() != 1) {
            error_at(e->loc, "read_borrow: requiere 1 argumento (borrow)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_b = lower_expr(e->args[0].get());
        if (v_b == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // El borrow es un host_ptr.  Marcamos is_host_ptr=true.
        // Para borrow derivado de unique<T>/shared<T> ya lo esta;
        // para `lend(local_plain)` el local fue promocionado a host
        // heap (RAW_ALLOC) en el lowering de su var-decl, asi que
        // el slot ES un host_ptr.  Param borrows tambien son host
        // por convencion (caller responsabilidad).
        fn_->values[v_b].is_host_ptr = true;
        // B2 fix: para STRUCT (y otros tipos cuyo "valor" SSA es PTR
        // a buffer: ARRAY/OPTIONAL/RESULT/CLASS), pass-through del
        // host_ptr al struct.  Sin esto, LOAD payload_t=PTR cargaria
        // los primeros 8 bytes del struct como si fueran otro ptr
        // (mismo bug que tenia el Deref).  Con pass-through, el
        // caller puede hacer (read_borrow(b)).x que baja a ADD off +
        // LOAD i32 correctamente.
        const Type inner = e->args[0]->result_type.pointee
                               ? *e->args[0]->result_type.pointee
                               : Type{};
        if (inner.kind == PrimitiveKind::STRUCT ||
            inner.kind == PrimitiveKind::ARRAY ||
            inner.kind == PrimitiveKind::OPTIONAL ||
            inner.kind == PrimitiveKind::RESULT ||
            inner.kind == PrimitiveKind::CLASS) {
            out_value = v_b;
            return true;
        }
        // LOAD T from [v_b] para tipos escalares.
        const ir::IrType payload_t = ir_type_from_primitive(inner.kind);
        const ir::IrValueId v_dst = fn_->new_value(payload_t);
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = payload_t;
        ld.dst = v_dst;
        ld.operands = {v_b};
        ld.source_line = e->loc.line;
        emit(current_block_, std::move(ld));
        out_value = v_dst;
        return true;
    }
    if (is_write_borrow) {
        if (e->args.size() != 2) {
            error_at(e->loc,
                     "write_borrow: requiere 2 argumentos (borrow_mut, value)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_b = lower_expr(e->args[0].get());
        const ir::IrValueId v_v = lower_expr(e->args[1].get());
        if (v_b == ir::IR_NO_VALUE || v_v == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        fn_->values[v_b].is_host_ptr = true;
        const Type inner = e->args[0]->result_type.pointee
                               ? *e->args[0]->result_type.pointee
                               : Type{};
        /* Un AGREGADO no cabe en un STORE.  Su valor SSA es un PUNTERO a su
         * buffer -- `read_borrow` de un struct hace pass-through del host_ptr,
         * arriba --, asi que un `STORE` escribia esa DIRECCION sobre los ocho
         * primeros bytes del propio struct.  Con `P {i32 x; i32 y;}` los dos
         * campos salian siendo las dos mitades de un puntero:
         *
         *     escribir(m): b.x = 99; b.y = 77; write_borrow(m, b);
         *     -> x=1136459712 y=569        (y distinto en cada ejecucion)
         *
         * Lo que toca es COPIAR los bytes.  El tamano sale del layout del
         * struct; si no se conoce, no se inventa: se avisa y no se emite una
         * escritura que corromperia el valor. */
        if (inner.kind == PrimitiveKind::STRUCT) {
            auto it_l = tc_.struct_layouts().find(inner.struct_name);
            if (it_l == tc_.struct_layouts().end()) {
                error_at(e->loc,
                         "write_borrow: no se conoce la disposicion de '" +
                             inner.struct_name +
                             "'; no se puede copiar el valor");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_n = emit_const(
                ir::IrType::I64,
                static_cast<uint64_t>(it_l->second.size_bytes), e->loc.line);
            fn_->values[v_v].is_host_ptr = true;
            ir::IrInstr mc{};
            mc.op = ir::IrOp::MEMCPY;
            mc.type = ir::IrType::I8;
            mc.dst = ir::IR_NO_VALUE;
            mc.operands = {v_b, v_v, v_n};
            mc.source_line = e->loc.line;
            emit(current_block_, std::move(mc));
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrType payload_t = ir_type_from_primitive(inner.kind);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = payload_t;
        st.operands = {v_v, v_b};
        st.source_line = e->loc.line;
        emit(current_block_, std::move(st));
        out_value = ir::IR_NO_VALUE;
        return true;
    }

    // ----- pid() -----
    // Devuelve el PID encoded del proceso actual via getpid r_dst.
    if (is_pid) {
        if (!e->args.empty()) {
            error_at(e->loc, "pid: no acepta argumentos");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        out_value = emit_getpid(e->loc.line);
        return true;
    }

    // ----- cpu_features() -> u64 (CPU dispatch, cimiento) -----
    // En native_poo_ (AOT): marca el uso (para wirear __vx_cpu_init en main),
    // asegura el global, y emite STR_LIT_ADDR(slot) + LOAD u64 (lectura del
    // bitmask que __vx_cpu_init dejo escrito al arranque).  En Full/interp
    // no hay cpuid native disponible -> devuelve 0 (consistente, sin error).
    if (is_cpu_features) {
        if (!e->args.empty()) {
            error_at(e->loc, "cpu_features: no acepta argumentos");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        if (!native_poo_) {
            // Path Full/interp/JIT: la VM corre sobre la CPU real -> emitimos
            // CALLN a la fn nativa `vesta_runtime:cpu_features` (registrada en
            // el virtual_lib_registry, sin DLL), que corre cpuid en el host y
            // devuelve el bitmask con el MISMO layout que el __vx_cpu_init de
            // AOT.  Resuelve igual en interp (loader/native_ffi) y en JIT
            // (auto_jit).  0 args, retorno u64 en R0.
            const int ln = e->loc.line;
            /* Un `cpuid` y devolver el bitmask: sin argumentos, sin memoria,
             * sin E/S.  Y determinista -- la CPU no cambia a mitad de
             * ejecucion --, que es lo que permite calcularlo UNA vez aunque se
             * consulte en un bucle. */
            {
                ir::IrNativeEffects fx;
                fx.declarados = true;
                out_mod_->register_native_import("vesta_runtime",
                                                 "cpu_features", fx);
            }
            ir::IrValueId v_feat = fn_->new_value(ir::IrType::U64);
            ir::IrInstr cl{};
            cl.op = ir::IrOp::CALLN;
            cl.type = ir::IrType::U64;
            cl.dst = v_feat;
            cl.func_name = "vesta_runtime:cpu_features";
            cl.operands = {};
            cl.source_line = ln;
            emit(current_block_, std::move(cl));
            out_value = v_feat;
            return true;
        }
        cpu_features_used_ = true;
        const uint64_t slot = ensure_cpu_features_global();
        const int ln = e->loc.line;
        ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr is{};
            is.op = ir::IrOp::STR_LIT_ADDR;
            is.type = ir::IrType::PTR;
            is.dst = v_addr;
            is.imm = slot;
            is.source_line = ln;
            emit(current_block_, std::move(is));
        }
        ir::IrValueId v_feat = fn_->new_value(ir::IrType::U64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::U64;
            ld.dst = v_feat;
            ld.operands = {v_addr};
            ld.source_line = ln;
            emit(current_block_, std::move(ld));
        }
        out_value = v_feat;
        return true;
    }

    // ----- args_count() -> i32 -----
    // Devuelve el numero de argumentos del script (vm->script_args.size()).
    // Baja a `getargc r_dst`, deposita uint64 que el caller trunca a i32.
    if (is_args_count) {
        if (!e->args.empty()) {
            error_at(e->loc, "args_count: no acepta argumentos");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // getargc devuelve i64 a nivel IR; truncamos a i32 si el caller lo
        // espera.
        ir::IrValueId v_n = emit_getargc(e->loc.line);
        v_n = cast_if_needed(v_n, ir::IrType::I64, ir::IrType::I32, e->loc.line,
                             /*is_explicit=*/true);
        out_value = v_n;
        return true;
    }

    // ----- Builtins de terminal / VT100 -----
    // Cada uno emite via vio_print una secuencia ANSI estatica.
    // term_move(row, col) requiere format dinamico: emite la secuencia
    // como una mezcla de prints y print_int.  Sin overhead extra
    // gracias al buffer global de 64 KB del plugin vesta_io (todos
    // los prints en una misma frame se agrupan en 1 syscall).

    // ----- getMethods(cls) / getFields(cls) -> i32 -----
    // Devuelve el numero de metodos / fields de instancia de la clase
    // via los opcodes existentes methodcount (0xDB) / fieldcount (0xDA).
    // Ambos depositan el count en R00 (no toman r_dst); capturamos a SSA.

    // ----- args_get(i) -> string -----
    // Devuelve un StringObject GC-managed con el contenido de args[i].
    // Baja a `getarg r_dst, r_idx`.  Si i fuera de rango, devuelve
    // GC_NULL_HANDLE = 0 (que el frontend trata como string nulo).
    if (is_args_get) {
        if (e->args.size() != 1 || !e->args[0]) {
            error_at(e->loc, "args_get: requiere 1 argumento (i32 indice)");
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_idx = lower_expr(e->args[0].get());
        if (v_idx == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        out_value = emit_getarg(v_idx, e->loc.line);
        return true;
    }

    // ----- msgsend(pid, value) -----
    // Reservamos 8 bytes en el frame del caller (alloca i8[8]),
    // escribimos `value` ahi como i64, y emitimos:
    //   msgsend r_pid, r_addr, r_len   (r_len = 8)
    // El opcode bytecode deposita 1/0 en R0 (ok flag), que capturamos
    // como i32 dst de la expresion.

    // No deberia alcanzarse: todos los builtins listados arriba estan
    // cubiertos.
    return false;
}

} // namespace vx
