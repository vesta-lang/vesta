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

    /* Cada familia atiende un grupo de builtins y son DISJUNTAS, asi que no
     * hace falta preguntarles por turno: la tabla dice cual es la suya y se va
     * derecho.  Antes se les preguntaba a las siete, y como cada una empieza
     * descartando los nombres que no son suyos, eso era recorrer las listas de
     * las seis que iban a decir que no.
     *
     * Una familia puede contestar que NO aunque el nombre sea suyo -- cuando
     * la forma de la llamada no encaja con ninguno de sus casos --, y entonces
     * el flujo sigue hacia abajo, al despacho general, igual que antes. */
    switch (builtin_family(b)) {
    case BuiltinFamily::Print:
        /* Imprimir: no es escribir sino averiguar QUE se escribe -- cada tipo
         * va distinto -- y con que forma.  Mas las secuencias del terminal,
         * que salen por la misma primitiva. */
        if (try_lower_print_builtins(e, b, out_value)) return true;
        break;
    case BuiltinFamily::Runtime:
        /* Lo que pide algo al MUNDO: ficheros, memoria del anfitrion, fibras,
         * modulos cargados en marcha.  Ninguno se resuelve dentro del
         * programa. */
        if (try_lower_runtime_builtins(e, b, out_value)) return true;
        break;
    case BuiltinFamily::Concurrent:
        /* Lo que supone que hay ALGUIEN MAS: memoria compartida, atomicos,
         * buzones y futuros.  Existen porque lo que uno escribe lo tiene que
         * ver el otro, y en el orden correcto. */
        if (try_lower_concurrent_builtins(e, b, out_value)) return true;
        break;
    case BuiltinFamily::Optional:
        /* Lo que puede NO estar: Optional y Result.  Los dos son la misma
         * idea -- un valor que lleva consigo si esta -- y ninguno toca el
         * monton. */
        if (try_lower_optional_builtins(e, b, out_value)) return true;
        break;
    case BuiltinFamily::Reflect:
        /* Preguntarle al programa por si mismo: la clase por su nombre, el
         * campo por el suyo, llamar sin saber a que hasta ese momento.  Es lo
         * contrario de todo lo demas, que se decide al compilar. */
        if (try_lower_reflect_builtins(e, b, out_value)) return true;
        break;
    case BuiltinFamily::Ownership:
        /* Quien es dueno de que y quien lo suelta.  Casi todo se decide al
         * COMPILAR: un prestamo son ocho bytes y sus reglas desaparecen del
         * codigo generado. */
        if (try_lower_ownership_builtins(e, b, out_value)) return true;
        break;
    case BuiltinFamily::String:
        /* Lo que se hace CON una cadena: medirla, cortarla, unirla,
         * compararla, sacarla al exterior.  Cada uno baja a UNA instruccion de
         * la maquina, no a una llamada. */
        if (try_lower_string_builtins(e, b, out_value)) return true;
        break;
    case BuiltinFamily::Math:
        /* Las operaciones matematicas.  Los numeros con decimales viajan a la
         * nativa como sus BITS metidos en un entero, porque la maquina pasa
         * los argumentos en registros de proposito general; las que devuelven
         * un entero NO hacen esa conversion. */
        if (try_lower_math_builtins(e, b, out_value)) return true;
        break;
    case BuiltinFamily::Other:
        /* Los que aun no tienen familia propia: caen al despacho de abajo. */
        break;
    }

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
            /* Caer al despacho normal: lo atiende la familia de
             * cadenas (lowering/builtins_string.cpp). */
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
    // panic("msg") -> opcode panic con FATAL_USER_ABORT.
    const bool is_panic = (name == "panic");
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
    const bool is_gensym_b = (name == "gensym");
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
        is_panic || is_gensym_b || is_static_assert_b ||
        is_col_ctor;
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

    // ----- panic("msg") -----
    // dispara FatalError(USER_ABORT, msg).  Capturable con
    // try/catch FatalError; si no hay handler, mata el proceso.
    // Acepta string literal (interna en static_data + emite panic
    // directo) o expresion string-typed (no soportado todavia).
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

    // ----- ptr_of(p) -----  T* host, sin consumir el smart pointer.
    // unique<T>: LOAD ptr from [slot]; resultado is_host_ptr.
    // shared<T>: LOAD ctrl from [slot]; ADD 16; resultado is_host_ptr.

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
