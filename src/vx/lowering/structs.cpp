/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/structs.cpp
 * @brief Bajada de los structs, que NO son clases.
 *
 * Un struct es un valor, no una referencia: vive donde se declaro -- en la pila
 * o dentro de quien lo contiene --, se copia al pasarlo, y muere sin que nadie
 * lo recoja.  De ahi que su bajada sea otra que la de una clase aunque las dos
 * tengan campos y metodos.
 *
 * Lo que eso obliga a resolver, y es lo que hay aqui: inicializar sus campos
 * (los que el usuario lista y los que no, que llevan su valor por defecto),
 * COPIAR el valor al pasarlo a una funcion -- y clonar lo que ese valor posea,
 * o dos duennos acabarian liberando lo mismo --, y llamar a un metodo sobre el
 * struct sin pasar por ninguna tabla, porque su tipo se sabe al compilar.
 *
 * Salvo cuando el usuario pide lo contrario: un struct puede declarar metodos
 * virtuales, y entonces necesita su tabla y un puntero a ella.  Eso tambien se
 * monta aqui, y es la unica parte que se parece a una clase.
 */
#include "util/env_flags.h"
#include "vx/lowering.h"
#include "util/thread_slot.h" // el estado por hilo NO va en thread_local
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
#include "lowering_internal.h" // la cocina compartida del lowering

namespace vx {

void Lowering::emit_struct_field_defaults(ir::IrValueId base_addr,
                                          const StructLayout &lay,
                                          uint32_t line,
                                          bool only_non_comptime) {
    for (const auto &fi : lay.fields) {
        // Filtro: saltar los defaults que la imagen comptime ya trae.
        if (only_non_comptime && fi.default_init && fi.bit_width == 0) {
            const ComptimeEvalResult dv =
                comptime_eval_expr(tc_, fi.default_init);
            if (dv.ok && !dv.deferred && !dv.is_str && !dv.is_array &&
                !dv.is_struct && !dv.is_type)
                continue;
        }
        // Direccion del campo (base + offset), heredando naturaleza host/VM.
        auto field_addr = [&]() -> ir::IrValueId {
            if (fi.offset == 0) return base_addr;
            ir::IrValueId v_off =
                emit_const(ir::IrType::I64, (uint64_t)fi.offset, line);
            ir::IrValueId v_a = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_a].is_host_ptr = fn_->values[base_addr].is_host_ptr;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_a;
            ad.operands = {base_addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
            return v_a;
        };
        if (!fi.default_init) {
            // Campo struct anidado SIN default propio: si su TIPO declara
            // defaults, aplicarlos recursivamente en la sub-direccion.
            if (fi.type.kind == PrimitiveKind::STRUCT) {
                auto it = tc_.struct_layouts().find(fi.type.struct_name);
                if (it != tc_.struct_layouts().end()) {
                    bool any_def = false;
                    for (const auto &sf : it->second.fields)
                        if (sf.default_init) {
                            any_def = true;
                            break;
                        }
                    if (any_def)
                        emit_struct_field_defaults(field_addr(), it->second,
                                                   line);
                }
            }
            continue;
        }
        // Campo escalar con default comptime-constante: lower + STORE.
        ir::IrValueId v_val = lower_expr(fi.default_init);
        if (v_val == ir::IR_NO_VALUE) continue;
        const ir::IrType ir_ft = ir_type_from_primitive(fi.type.kind);
        v_val = cast_if_needed(v_val, fn_->values[v_val].type, ir_ft, line,
                               /*is_explicit=*/true);
        ir::IrValueId v_addr = field_addr();
        if (fi.bit_width > 0) {
            // Bit field con default: por ahora se ignora (raro); el zero-fill
            // deja el campo a 0.  Un default de bit field requeriria RMW.
            continue;
        }
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir_ft;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_val, v_addr};
        st.source_line = line;
        emit(current_block_, std::move(st));
    }
}

void Lowering::emit_struct_init_fields(ir::IrValueId base_addr,
                                       const StructLayout &lay,
                                       ast::InitListExpr *il, uint32_t line) {
    // Aplicar primero los valores por defecto de los campos; el init-list
    // explicito de abajo sobrescribe los campos que liste (DSE limpia lo
    // muerto).
    emit_struct_field_defaults(base_addr, lay, line);
    for (size_t i = 0; i < il->elements.size(); ++i) {
        const StructFieldInfo *fi = nullptr;
        if (il->is_designated) {
            const std::string &fname = il->field_names[i];
            for (const auto &f : lay.fields) {
                if (f.name == fname) {
                    fi = &f;
                    break;
                }
            }
            if (!fi) {
                error_at(il->loc, "lowering: campo '" + fname +
                                      "' no existe en struct '" + lay.name +
                                      "'");
                continue;
            }
        } else {
            if (i >= lay.fields.size()) {
                error_at(il->loc,
                         "lowering: init list excede campos del struct");
                break;
            }
            fi = &lay.fields[i];
        }
        // Direccion del campo destino (base + offset).
        ir::IrValueId v_addr = base_addr;
        if (fi->offset > 0) {
            ir::IrValueId v_off =
                emit_const(ir::IrType::I64, (uint64_t)fi->offset, line);
            v_addr = fn_->new_value(ir::IrType::PTR);
            // `base + off` sigue apuntando a la MISMA memoria que `base`: la
            // naturaleza (host / VM) se hereda.  Sin esto, un struct en host
            // inicializado con una init-list anidada escribia sus campos con
            // `mov` (VM) sobre una direccion host -> basura.
            fn_->values[v_addr].is_host_ptr =
                fn_->values[base_addr].is_host_ptr;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_addr;
            ad.operands = {base_addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        ast::Expr *elem = il->elements[i].get();
        // Campo de tipo STRUCT inicializado con un init-list ANIDADO
        // (`{.min = {.x=.., .y=..}}` o `{.min = Punto{...}}`): se rellena
        // RECURSIVAMENTE in-place en la direccion del campo.  lower_expr no
        // baja un InitListExpr como valor, por eso hay que tratarlo aqui.
        if (fi->type.kind == PrimitiveKind::STRUCT &&
            elem->kind == ast::NodeKind::InitListExpr) {
            auto it_sl = tc_.struct_layouts().find(fi->type.struct_name);
            if (it_sl == tc_.struct_layouts().end()) {
                error_at(il->loc, "lowering: struct '" + fi->type.struct_name +
                                      "' sin layout (init anidado)");
                continue;
            }
            emit_struct_init_fields(v_addr, it_sl->second,
                                    static_cast<ast::InitListExpr *>(elem),
                                    line);
            continue;
        }
        ir::IrValueId v_val = lower_expr(elem);
        if (v_val == ir::IR_NO_VALUE) continue;
        // Campo AGREGADO inline (struct/array) desde una EXPRESION (otra
        // variable, llamada, ...): copia memberwise desde la direccion origen
        // (no un STORE escalar, que guardaria la direccion como puntero).
        // Un campo de tipo `@overlay struct` NO es un agregado inline: guarda
        // el HANDLE de la vista (8 bytes) -> STORE escalar del puntero (abajo).
        if ((fi->type.kind == PrimitiveKind::STRUCT &&
             !type_is_overlay(fi->type)) ||
            fi->type.kind == PrimitiveKind::ARRAY) {
            uint64_t sz = size_of_type(fi->type);
            if (sz == 0 && fi->type.kind == PrimitiveKind::STRUCT) {
                auto it_sl = tc_.struct_layouts().find(fi->type.struct_name);
                if (it_sl != tc_.struct_layouts().end())
                    sz = (uint64_t)it_sl->second.size_bytes;
            }
            if (sz == 0) sz = 8;
            emit_memberwise_copy(v_addr, v_val, sz, line);
            if (fi->type.kind == PrimitiveKind::STRUCT) {
                auto it_sl = tc_.struct_layouts().find(fi->type.struct_name);
                if (it_sl != tc_.struct_layouts().end() &&
                    it_sl->second.has_copy_hook) {
                    emit_struct_method_on_host_field(
                        v_addr, fi->type.struct_name,
                        fi->type.struct_name + "____clone__", line);
                }
            }
            continue;
        }
        const ir::IrType ir_ft = ir_type_from_primitive(fi->type.kind);
        const bool elem_is_literal =
            elem->kind == ast::NodeKind::IntLitExpr ||
            elem->kind == ast::NodeKind::FloatLitExpr ||
            elem->kind == ast::NodeKind::BoolLitExpr ||
            elem->kind == ast::NodeKind::CharLitExpr ||
            elem->kind == ast::NodeKind::NullLitExpr;
        v_val = cast_if_needed(v_val, fn_->values[v_val].type, ir_ft, line,
                               /*is_explicit=*/elem_is_literal);
        if (fi->bit_width > 0) {
            error_at(il->loc, "lowering: init list no soporta bit fields aun");
            continue;
        }
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir_ft;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_val, v_addr};
        st.source_line = line;
        emit(current_block_, std::move(st));
    }
}

void Lowering::emit_struct_method_on_host_field(ir::IrValueId field_addr,
                                                const std::string &struct_name,
                                                const std::string &method_label,
                                                uint32_t line) {
    // Historico: cuando un metodo de struct se compilaba con this=VM en
    // interp/JIT, un campo en el payload HOST de una clase habia que COPIARLO a
    // un temp VM para poder llamarlo.  Ese rodeo ya no hace falta -- `this` es
    // host en los tres modos (ver lower_struct_methods) -- y ademas era DANINO
    // para los metodos que MUTAN: el dtor o el `__clone__` operaban sobre la
    // COPIA, asi que el refcount real no se tocaba (el free nunca llegaba, o
    // llegaba de mas).  El CALL va directo sobre el campo.
    const bool need_temp = false;
    if (!need_temp) {
        ir::IrInstr cd{};
        cd.op = ir::IrOp::CALL;
        cd.type = ir::IrType::VOID;
        cd.dst = ir::IR_NO_VALUE;
        cd.operands = {field_addr};
        cd.func_name = method_label;
        cd.source_line = line;
        emit(current_block_, std::move(cd));
        return;
    }
    // interp/JIT: copiar el campo (host) a un temporal VM-stack y llamar el
    // metodo sobre el temporal (this VM == lo que el metodo asume).
    uint64_t sz = 8;
    auto it_sl = tc_.struct_layouts().find(struct_name);
    if (it_sl != tc_.struct_layouts().end())
        sz = static_cast<uint64_t>(it_sl->second.size_bytes);
    if (sz == 0) sz = 8;
    // ALLOCA temp VM (is_host_ptr = false).
    const ir::IrValueId tmp = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.imm = sz;
        al.dst = tmp;
        al.source_line = line;
        emit(current_block_, std::move(al));
    }
    // memcpy field_addr (host) -> tmp (VM): qword por qword.
    const uint64_t qwords = (sz + 7) / 8;
    for (uint64_t qi = 0; qi < qwords; ++qi) {
        const ir::IrValueId v_off =
            emit_const(ir::IrType::I64, static_cast<int64_t>(qi * 8), line);
        const ir::IrValueId src_at = fn_->new_value(ir::IrType::PTR);
        fn_->values[src_at].is_host_ptr = true; // campo en payload host
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = src_at;
            ad.operands = {field_addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        const ir::IrValueId word = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = word;
            ld.operands = {src_at};
            ld.source_line = line;
            emit(current_block_, std::move(ld));
        }
        const ir::IrValueId dst_at = fn_->new_value(ir::IrType::PTR);
        // tmp es VM (is_host_ptr = false por defecto).
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = dst_at;
            ad.operands = {tmp, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.operands = {word, dst_at};
            st.source_line = line;
            emit(current_block_, std::move(st));
        }
    }
    // CALL method_label(tmp).
    {
        ir::IrInstr cd{};
        cd.op = ir::IrOp::CALL;
        cd.type = ir::IrType::VOID;
        cd.dst = ir::IR_NO_VALUE;
        cd.operands = {tmp};
        cd.func_name = method_label;
        cd.source_line = line;
        emit(current_block_, std::move(cd));
    }
}

ir::IrValueId Lowering::emit_struct_arg_copy_clone(
    ir::IrValueId v_src, const std::string &struct_name, uint32_t line) {
    // Ownership ruta B (H1 paso por valor): pasar un struct CON copy-hook por
    // valor a una funcion (`f(a)`) hace una COPIA -- la callee recibe su propia
    // instancia.  Alocamos una copia (ALLOCA, misma memory class que un struct
    // local: VM en interp/JIT, host en AOT/native_poo), memcpy del origen, y
    // `copia.__clone__()` para que el tipo gestionado ajuste su recurso (p.ej.
    // ++refcount).  Devuelve la direccion de la copia para pasarla al CALL.  El
    // caller emite el `~dtor` de la copia tras el CALL (la callee no posee el
    // param, igual que cualquier struct por valor en Vesta).
    uint64_t sz = 8;
    auto it_sl = tc_.struct_layouts().find(struct_name);
    if (it_sl != tc_.struct_layouts().end())
        sz = static_cast<uint64_t>(it_sl->second.size_bytes);
    if (sz == 0) sz = 8;
    // ALLOCA copia: host-ness identica a un struct local, o sea HOST en los
    // tres modos (ver lower_var_decl).  Cuando esta copia se quedaba en la pila
    // VM en interp/JIT, el callee -- que lee sus params agregados con `movh` --
    // la leia como basura, y su `__clone__` / `~dtor` operaban sobre esa
    // basura.
    const ir::IrValueId copy = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.imm = sz;
        al.dst = copy;
        al.host_alloca = true;
        al.source_line = line;
        emit(current_block_, std::move(al));
    }
    fn_->values[copy].is_host_ptr = true;
    // memcpy v_src -> copy (respetando host-ness de origen y destino).
    const bool src_is_host = fn_->values[v_src].is_host_ptr;
    const bool dst_is_host = fn_->values[copy].is_host_ptr;
    const uint64_t qwords = (sz + 7) / 8;
    for (uint64_t qi = 0; qi < qwords; ++qi) {
        const ir::IrValueId v_off =
            emit_const(ir::IrType::I64, static_cast<int64_t>(qi * 8), line);
        const ir::IrValueId src_at = fn_->new_value(ir::IrType::PTR);
        fn_->values[src_at].is_host_ptr = src_is_host;
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = src_at;
            ad.operands = {v_src, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        const ir::IrValueId word = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = word;
            ld.operands = {src_at};
            ld.source_line = line;
            emit(current_block_, std::move(ld));
        }
        const ir::IrValueId dst_at = fn_->new_value(ir::IrType::PTR);
        fn_->values[dst_at].is_host_ptr = dst_is_host;
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = dst_at;
            ad.operands = {copy, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.operands = {word, dst_at};
            st.source_line = line;
            emit(current_block_, std::move(st));
        }
    }
    // copia.__clone__()  (this = copy, misma memory class -> sin mismatch).
    emit_struct_method_on_host_field(copy, struct_name,
                                     struct_name + "__" + "__clone__", line);
    return copy;
}

ir::IrValueId Lowering::lower_struct_method_call(ast::CallExpr *e) {
    // s.method(args) sobre un struct value-type.  Bajamos a un CALL
    // directo a <Struct>__<metodo>(struct_addr, [retbuf], args...).
    // El SSA value del struct (fa->base) ES la direccion del buffer:
    // un ALLOCA en VM-stack para `S s;`, o un host_ptr si el struct
    // vive en host memory (malloc / ptr_of).  No tocamos is_host_ptr:
    // el callee recibe el ptr tal cual (el metodo se compila con
    // 'this' en memoria VM por defecto; structs en VM-stack son el
    // caso comun y dominante).
    auto *fa = static_cast<ast::FieldAccessExpr *>(e->callee.get());
    Type bt = fa->base->result_type;
    // @Virtual: `ptr.metodo()` sobre un `Struct*` -> el struct es el pointee, y
    // el `this` es el VALOR del puntero (la direccion del objeto), no la de un
    // ALLOCA.  lower_expr(fa->base) ya da ese valor.
    if (bt.kind == PrimitiveKind::PTR && bt.pointee &&
        bt.pointee->kind == PrimitiveKind::STRUCT)
        bt = *bt.pointee;
    auto it = tc_.struct_layouts().find(bt.struct_name);
    if (it == tc_.struct_layouts().end()) {
        error_at(e->loc,
                 "lowering: struct desconocido '" + bt.struct_name + "'");
        return ir::IR_NO_VALUE;
    }
    const StructLayout &lay = it->second;
    const ClassMethodInfo *mtd = nullptr;
    for (const auto &m : lay.methods) {
        if (m.name == fa->field_name) {
            mtd = &m;
            break;
        }
    }
    if (!mtd) {
        error_at(e->loc, "lowering: metodo '" + fa->field_name +
                             "' no encontrado en struct '" + bt.struct_name +
                             "'");
        return ir::IR_NO_VALUE;
    }

    // Direccion del struct (= this).
    const ir::IrValueId this_addr = lower_expr(fa->base.get());
    if (this_addr == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

    // Bajar argumentos (con auto-promocion de string literales).
    std::vector<ir::IrValueId> arg_vals;
    arg_vals.reserve(e->args.size());
    for (size_t ai = 0; ai < e->args.size(); ++ai) {
        auto &a = e->args[ai];
        if (!a) return ir::IR_NO_VALUE;
        const bool param_is_string =
            ai < mtd->param_types.size() &&
            mtd->param_types[ai].kind == PrimitiveKind::STRING;
        if (param_is_string && a->kind == ast::NodeKind::StringLitExpr) {
            auto *slit = static_cast<ast::StringLitExpr *>(a.get());
            const ir::IrValueId av =
                lower_string_literal_to_string_object(slit);
            if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            arg_vals.push_back(av);
            continue;
        }
        const ir::IrValueId av = lower_expr(a.get());
        if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        arg_vals.push_back(av);
    }

    // SRET: si el metodo devuelve Optional/Result, el caller aloca el
    // retbuf (host_alloca para que callee/caller usen movh) y lo pasa
    // como segundo "arg" (tras this).  El dst del CALL es VOID; el
    // valor visible es el retbuf.
    // native_poo_: un metodo que devuelve `string` value-type usa SRET
    // (simetrico con el callee en lower_struct_methods).  El caller aloca el
    // retbuf de 24 bytes en host-stack y lo pasa tras 'this'.
    // STRUCT por valor: mismo motivo que Optional/Result -- el buffer del
    // struct vive en el frame del callee y muere al RET.  Un `@overlay struct`
    // no: su valor ES un puntero de 8 bytes, va por registro.
    const StructLayout *ret_slay = nullptr;
    if (mtd->return_type.kind == PrimitiveKind::STRUCT &&
        !mtd->return_type.struct_name.empty()) {
        const auto &elays = tc_.enum_layouts();
        if (elays.find(mtd->return_type.struct_name) == elays.end()) {
            auto it_rs =
                tc_.struct_layouts().find(mtd->return_type.struct_name);
            if (it_rs != tc_.struct_layouts().end() &&
                !it_rs->second.is_overlay)
                ret_slay = &it_rs->second;
        }
    }
    const bool method_sret =
        (mtd->return_type.kind == PrimitiveKind::OPTIONAL ||
         mtd->return_type.kind == PrimitiveKind::RESULT ||
         (native_poo_ && mtd->return_type.kind == PrimitiveKind::STRING) ||
         ret_slay != nullptr);
    ir::IrValueId v_retbuf = ir::IR_NO_VALUE;
    if (method_sret) {
        const uint64_t buf_bytes =
            ret_slay != nullptr
                ? ((static_cast<uint64_t>(ret_slay->size_bytes) + 7ULL) & ~7ULL)
            : (mtd->return_type.kind == PrimitiveKind::OPTIONAL)
                ? (uint64_t)optional_buf_bytes(mtd->return_type)
                : 24ULL;
        v_retbuf = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.imm = buf_bytes;
        al.dst = v_retbuf;
        al.host_alloca = true;
        al.source_line = e->loc.line;
        emit(current_block_, std::move(al));
        fn_->values[v_retbuf].is_host_ptr = true;
    }

    const ir::IrType ret_ir_decl =
        ir_type_from_primitive(mtd->return_type.kind);
    const ir::IrType ret_ir = method_sret ? ir::IrType::VOID : ret_ir_decl;
    const ir::IrValueId dst =
        (ret_ir == ir::IrType::VOID) ? ir::IR_NO_VALUE : fn_->new_value(ret_ir);
    if (dst != ir::IR_NO_VALUE) {
        const PrimitiveKind rk = mtd->return_type.kind;
        if (rk == PrimitiveKind::CLASS) {
            fn_->values[dst].is_host_ptr = true;
            fn_->values[dst].is_gc_object = true;
        } else if ((rk == PrimitiveKind::PTR || rk == PrimitiveKind::ARRAY) &&
                   !mtd->return_type.is_virtual) {
            fn_->values[dst].is_host_ptr = true;
        }
    }

    // Operandos del CALL: this_addr, [retbuf], args...
    std::vector<ir::IrValueId> operands;
    operands.reserve(arg_vals.size() + 2);
    operands.push_back(this_addr);
    if (method_sret) operands.push_back(v_retbuf);
    for (auto av : arg_vals)
        operands.push_back(av);

    if (mtd->is_virtual) {
        // @Virtual: dispatch DINAMICO por vtable.  El vptr (offset 0 del
        // objeto) apunta a la vtable del tipo REAL; el slot da la
        // implementacion.  Es correcto tanto por Base* (tipo dinamico) como por
        // valor concreto (el vptr se fija a la vtable del concreto en la
        // construccion).  La devirtualizacion a CALL directo cuando el tipo es
        // estatico y concreto es una optimizacion posterior.
        const uint32_t slot = mtd->vtable_index;
        // %vptr = LOAD [this_addr + 0]  (this es host -> movh recupera el vptr,
        // que es una direccion VM de la vtable en la seccion de codigo).
        const ir::IrValueId v_vptr = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_vptr;
            ld.operands = {this_addr};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
        }
        // %fnaddr = %vptr + slot*8  (%vptr es VM -> load de la entrada es mov
        // VM)
        ir::IrValueId v_fnaddr = v_vptr;
        if (slot != 0) {
            const ir::IrValueId v_off =
                emit_const(ir::IrType::I64, (uint64_t)slot * 8u, e->loc.line);
            v_fnaddr = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_fnaddr;
            ad.operands = {v_vptr, v_off};
            ad.source_line = e->loc.line;
            emit(current_block_, std::move(ad));
        }
        // %fn = LOAD [%fnaddr]  (cfn: direccion del metodo; slot host -> movh)
        const ir::IrValueId v_fn = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_fn;
            ld.operands = {v_fnaddr};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
        }
        // CALLIND %fn(this_addr, [retbuf], args...)
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLIND;
        ins.type = ret_ir;
        ins.dst = dst;
        ins.func_ptr = v_fn;
        ins.operands = std::move(operands);
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        return method_sret ? v_retbuf : dst;
    }

    ir::IrInstr ins{};
    ins.op = ir::IrOp::CALL;
    ins.type = ret_ir;
    ins.dst = dst;
    // Metodo IMPORTADO cross-module: usar el simbolo real del .velb origen
    // (link_name, p.ej. "std__wideint__u128____div__"); reconstruir
    // "<struct_local>__<metodo>" llevaria el mangling del consumidor y el
    // linker no lo resolveria.  Metodos del propio modulo: link_name vacio ->
    // el label clasico.
    ins.func_name = mtd->link_name.empty()
                        ? (bt.struct_name + "__" + fa->field_name)
                        : mtd->link_name;
    ins.operands = std::move(operands);
    ins.source_line = e->loc.line;
    emit(current_block_, std::move(ins));

    return method_sret ? v_retbuf : dst;
}

uint64_t Lowering::get_or_emit_struct_vtable(const StructLayout &lay) {
    auto cit = struct_vtable_didx_.find(lay.name);
    if (cit != struct_vtable_didx_.end()) return cit->second;

    // Numero de slots = max(vtable_index)+1 sobre los metodos virtuales.
    uint32_t nslots = 0;
    for (const auto &mi : lay.methods)
        if (mi.is_virtual && mi.vtable_index + 1u > nslots)
            nslots = mi.vtable_index + 1u;
    // Blob de nslots*8 bytes a cero; cada slot recibe una reloc ABS64 al
    // simbolo del metodo (<owner>__<metodo>) que lo ocupa.  El owner es la
    // clase que DEFINE el metodo tras el aplanado (defining_class = este
    // struct, porque el flatten reescribe los heredados con el nombre del
    // derivado -> el override gana su slot con el simbolo del derivado).
    std::vector<uint8_t> vt(static_cast<size_t>(nslots) * 8u, 0);
    const uint64_t idx = out_mod_->static_data.push_back(std::move(vt));
    auto &vm = out_mod_->static_data.meta_at(idx);
    vm.section_name = ".data.rel.ro"; // RELRO como las vtables de C++
    vm.flags |=
        ir::IrModule::SD_FLAG_FORCE_EMIT | ir::IrModule::SD_FLAG_NON_DEDUP;
    for (const auto &mi : lay.methods) {
        if (!mi.is_virtual) continue;
        const std::string owner =
            mi.defining_class.empty() ? lay.name : mi.defining_class;
        ir::IrModule::StaticDataMeta::SymRef sr;
        sr.offset = mi.vtable_index * 8u;
        sr.sym = owner + "__" + mi.name; // reloc datos->codigo
        sr.width = 8;
        sr.is_rel = 0;
        vm.sym_refs.push_back(std::move(sr));
    }
    struct_vtable_didx_[lay.name] = idx;
    return idx;
}

void Lowering::emit_struct_vptr_init(ir::IrValueId struct_addr,
                                     const StructLayout &lay, uint32_t line) {
    if (!lay.is_polymorphic) return;
    const uint64_t vt_idx = get_or_emit_struct_vtable(lay);
    // %vt = &vtable (STR_LIT_ADDR del blob).  La vtable vive en la seccion de
    // CODIGO (direccion VM en interp/JIT; .rodata en AOT), como un string
    // literal -> NO is_host_ptr.  El struct SI es host (host_alloca): el STORE
    // del vptr a [struct_addr+0] usa movh porque struct_addr es host, pero el
    // VALOR guardado (la direccion de la vtable) es VM.  Al leer el vptr
    // (load [struct_addr] = movh) se recupera esa direccion VM, y el load de la
    // entrada (load [vptr] = mov VM) lee la vtable correctamente.
    const ir::IrValueId v_vt = fn_->new_value(ir::IrType::PTR);
    ir::IrInstr sa{};
    sa.op = ir::IrOp::STR_LIT_ADDR;
    sa.type = ir::IrType::PTR;
    sa.dst = v_vt;
    sa.imm = vt_idx;
    sa.source_line = line;
    emit(current_block_, std::move(sa));
    // STORE %vt -> [struct_addr + 0]  (el vptr).
    ir::IrInstr st{};
    st.op = ir::IrOp::STORE;
    st.type = ir::IrType::I64;
    st.dst = ir::IR_NO_VALUE;
    st.operands = {v_vt, struct_addr};
    st.source_line = line;
    emit(current_block_, std::move(st));
}

ir::IrValueId Lowering::lower_super_call_expr(ast::SuperCallExpr *e) {
    // Resolver this implicito.
    const ir::IrValueId v_this = lookup("this");
    if (v_this == ir::IR_NO_VALUE) {
        error_at(e->loc, "super(...): no se encontro 'this' en el scope");
        return ir::IR_NO_VALUE;
    }
    // Buscar el super_name del current_class_.
    if (current_class_lowering_.empty()) {
        error_at(e->loc, "super(...) fuera de cuerpo de clase");
        return ir::IR_NO_VALUE;
    }
    auto it = tc_.class_layouts().find(current_class_lowering_);
    if (it == tc_.class_layouts().end() || it->second.super_name.empty()) {
        error_at(e->loc, "super(...) en clase sin super");
        return ir::IR_NO_VALUE;
    }
    const std::string &super_name = it->second.super_name;
    auto it_s = tc_.class_layouts().find(super_name);
    if (it_s == tc_.class_layouts().end()) {
        error_at(e->loc, "super clase '" + super_name + "' desconocida");
        return ir::IR_NO_VALUE;
    }
    // Buscar el ctor PROPIO del super (no heredado de su super-super).
    // BugFix R1.fix: si el super tambien deriva de otra clase, sus
    // methods comienzan con la inherited ctor del super-super.  Sin
    // priorizar el ctor cuyo defining_class == super_name, el callsuper
    // dispatcharia a Mid.vtable[0] = Base.__ctor (inherited) en lugar
    // de Mid.__ctor (own), con la aridad de Base.__ctor en vez de Mid.
    const ClassMethodInfo *super_ctor = nullptr;
    for (const auto &m : it_s->second.methods) {
        if (m.is_constructor && m.defining_class == super_name) {
            super_ctor = &m;
            break;
        }
    }
    // Fallback: si no hay ctor propio en super, usar el primero.
    if (!super_ctor) {
        for (const auto &m : it_s->second.methods) {
            if (m.is_constructor) {
                super_ctor = &m;
                break;
            }
        }
    }
    if (!super_ctor) {
        error_at(e->loc, "super(...): la clase super '" + super_name +
                             "' no tiene constructor");
        return ir::IR_NO_VALUE;
    }
    // Bajar args.
    std::vector<ir::IrValueId> arg_vals;
    arg_vals.reserve(e->args.size());
    for (auto &a : e->args) {
        const ir::IrValueId av = lower_expr(a.get());
        if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        arg_vals.push_back(av);
    }
    // AOT (native_poo_): la clase super es estaticamente conocida -> CALL
    // DIRECTO a <super>__ctor(this, args).  Evita findclass + callsuper
    // (ambos runtime, no compilables en bare).  Habilita herencia en AOT.
    if (native_poo_) {
        const std::string sname = super_ctor->defining_class.empty()
                                      ? super_name
                                      : super_ctor->defining_class;
        ir::IrInstr ca{};
        ca.op = ir::IrOp::CALL;
        ca.type = ir::IrType::VOID;
        ca.dst = ir::IR_NO_VALUE;
        ca.func_name = sname + "__ctor";
        ca.operands.push_back(v_this);
        for (auto av : arg_vals)
            ca.operands.push_back(av);
        ca.source_line = e->loc.line;
        emit(current_block_, std::move(ca));
        return ir::IR_NO_VALUE;
    }
    // Resolver ClassInfo* del super via findclass inline (mismo patron
    // que forName).  Resultado en v_cls.  Luego emitir CALLSUPER IR.
    const uint64_t super_name_idx = intern_class_name(*out_mod_, super_name);
    const uint32_t super_name_len = static_cast<uint32_t>(super_name.size());
    // Sprint 5: findclass via IR ops.
    const ir::IrValueId v_cls =
        emit_findclass_by_name(super_name_idx, super_name_len, e->loc.line);
    // Emit CALLSUPER IR: layout = [cls, this, args...], imm=vtbl_idx.
    // El emisor IR coloca obj en r1, args en r2..r_{N+1}, cls en r13,
    // y emite `callsuper r13, vtable_idx`.  Sin RAW_ASM: el regalloc,
    // DCE y otros pases ven la operacion como un CALL real.
    ir::IrInstr cs{};
    cs.op = ir::IrOp::CALLSUPER;
    cs.type = ir::IrType::VOID;
    cs.dst = ir::IR_NO_VALUE;
    cs.operands.push_back(v_cls);
    cs.operands.push_back(v_this);
    for (auto av : arg_vals)
        cs.operands.push_back(av);
    cs.imm = static_cast<uint64_t>(super_ctor->vtable_index);
    cs.source_line = e->loc.line;
    emit(current_block_, std::move(cs));
    return ir::IR_NO_VALUE;
}


} // namespace vx
