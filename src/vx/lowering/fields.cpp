/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/fields.cpp
 * @brief Llegar a un campo: el de un objeto, y el de una VISTA sobre bytes.
 *
 * Un campo de clase se lee y se escribe por su desplazamiento, que se sabe al
 * compilar.  Una VISTA -- un overlay -- es otra cosa: describe una estructura
 * que ya esta en memoria, escrita por otro (una cabecera de un ejecutable, un
 * paquete de red), y ahi el desplazamiento de un campo puede depender de lo que
 * digan los campos ANTERIORES, y su orden de bytes puede no ser el de esta
 * maquina.
 *
 * Por eso van juntos: son la misma pregunta -- donde estan los bytes de este
 * campo -- con dos respuestas de dificultad muy distinta.  Para la vista hay
 * que generar codigo que resuelva el desplazamiento en ejecucion, calcular
 * hasta donde llega, y voltear los bytes cuando toque.
 */
#include "vx/lowering.h"
#include "util/thread_slot.h" // el estado por hilo NO va en thread_local
#include "ir/ir_type_info.h" // vocabulario UNICO de anchura/clase de un IrType
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include "lowering_internal.h" // la cocina compartida del lowering

namespace vx {
ir::IrValueId Lowering::lower_overlay_root(ast::Expr *e) {
    // Camina hasta la vista raiz: la base mas profunda que NO es un acceso a
    // campo/elemento overlay (p.ej. `pe` en `pe.Imports[i].name`).
    ast::Expr *root = e;
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
    return lower_expr(root);
}

std::string Lowering::generate_overlay_resolver(const StructLayout &lay,
                                                const StructFieldInfo &fi,
                                                bool is_element) {
    const std::string fn_name =
        (is_element ? "__ovl_element_" : "__ovl_resolve_") + lay.name + "_" +
        fi.name;
    if (generated_overlay_resolvers_.count(fn_name)) return fn_name; // dedup
    // Insertar YA el nombre: un resolver @element puede RECURSAR
    // (`self.Name[index-1]`); sin esto la generacion compile-time no
    // terminaria. La recursion se vuelve un CALL runtime al mismo resolver.
    generated_overlay_resolvers_.insert(fn_name);
    ast::BlockStmt *body_block =
        is_element ? fi.element_block : fi.offset_block;

    // Salvar contexto del padre (mismo protocolo que generate_lambda_helper).
    /* El guarda se lleva el contexto del padre y lo devuelve al salir. */
    ChildFunctionScope parent(*this);
    const bool saved_sret_active = sret_active_;
    const ir::IrValueId saved_sret_retbuf = sret_retbuf_;
    const uint64_t saved_sret_buf_size = sret_buf_size_;
    const bool saved_returns_fn = current_fn_returns_function_;
    sret_active_ = false;
    sret_retbuf_ = ir::IR_NO_VALUE;
    sret_buf_size_ = 0;
    current_fn_returns_function_ = false;

    ir::IrFunction child_fn;
    child_fn.name = fn_name;
    child_fn.ret_type = ir::IrType::PTR; // devuelve una DIRECCION (host)

    // Param `self` = puntero base de la vista (host).
    ir::IrValueId self_pv = child_fn.new_value(ir::IrType::PTR, "%self");
    child_fn.values[self_pv].is_param = true;
    child_fn.values[self_pv].is_host_ptr = true;
    child_fn.params.push_back(self_pv);
    // @element: 2o param `index` (i64) = el elemento a resolver.  Orden de
    // params: self, [index], [root].
    ir::IrValueId index_pv = ir::IR_NO_VALUE;
    if (is_element) {
        index_pv = child_fn.new_value(ir::IrType::I64, "%index");
        child_fn.values[index_pv].is_param = true;
        child_fn.params.push_back(index_pv);
    }
    // F4: si el resolver usa parent<T>(), un param `root` = puntero de la vista
    // RAIZ (el call site lo enhebra caminando la cadena de accesos).
    ir::IrValueId root_pv = ir::IR_NO_VALUE;
    if (fi.resolver_uses_parent) {
        root_pv = child_fn.new_value(ir::IrType::PTR, "%root");
        child_fn.values[root_pv].is_param = true;
        child_fn.values[root_pv].is_host_ptr = true;
        child_fn.params.push_back(root_pv);
    }

    const ir::IrBlockId entry = child_fn.new_block("entry");
    fn_ = &child_fn;
    current_block_ = entry;
    push_scope();

    // `base` = self; cada campo hermano de offset CONSTANTE se lee de
    // [self + off] (host) y se liga por nombre -> el body los usa como locales.
    // F4: `this`/`self` = la vista completa (el propio puntero base), para que
    // el resolver navegue arrays hermanos declarativamente
    // (`this.Sections[i].campo`) via la maquinaria overlay -- sin aritmetica de
    // punteros ni helpers.
    bind("base", self_pv);
    bind("this", self_pv);
    // F4: `parent<T>()` en el body baja a este valor (el puntero raiz).
    if (root_pv != ir::IR_NO_VALUE) bind("__ovl_root", root_pv);
    // @element: `index` en scope.
    if (index_pv != ir::IR_NO_VALUE) bind("index", index_pv);
    for (const auto &sib : lay.fields) {
        // Saltar dinamicos y ARRAYS: los arrays no son un escalar cargable; se
        // navegan por `this.<array>[i]` (no como nombre desnudo).
        if (sib.offset_expr || sib.offset_block || sib.array_count ||
            sib.array_stride || sib.element_block)
            continue; // solo escalares de offset constante
        ir::IrValueId saddr = self_pv;
        if (sib.offset != 0) {
            ir::IrValueId so =
                emit_const(ir::IrType::I64, (uint64_t)sib.offset, 0);
            saddr = child_fn.new_value(ir::IrType::PTR);
            child_fn.values[saddr].is_host_ptr = true;
            ir::IrInstr a{};
            a.op = ir::IrOp::ADD;
            a.type = ir::IrType::PTR;
            a.dst = saddr;
            a.operands = {self_pv, so};
            child_fn.append(entry, std::move(a));
        }
        const ir::IrType st = ir_type_from_primitive(sib.type.kind);
        ir::IrValueId sv = child_fn.new_value(st);
        ir::IrInstr l{};
        l.op = ir::IrOp::LOAD;
        l.type = st;
        l.dst = sv;
        l.operands = {saddr};
        child_fn.append(entry, std::move(l));
        bind(sib.name, sv);
    }

    // Lower del body: if/else, multiples return, etc. -> RET (la direccion).
    lower_block(body_block);
    if (!block_terminated_) {
        // Defensa: si el body no termina en return, devolvemos base
        // (identidad).
        ir::IrInstr rt{};
        rt.op = ir::IrOp::RET;
        rt.type = ir::IrType::PTR;
        rt.operands = {self_pv};
        emit(current_block_, std::move(rt));
        block_terminated_ = true;
    }

    pop_scope();
    pending_spawn_helpers_.push_back(std::move(child_fn));
    generated_overlay_resolvers_.insert(fn_name);

    // Restaurar contexto del padre.
    sret_active_ = saved_sret_active;
    sret_retbuf_ = saved_sret_retbuf;
    sret_buf_size_ = saved_sret_buf_size;
    current_fn_returns_function_ = saved_returns_fn;
    return fn_name;
}

std::string Lowering::generate_overlay_extent(const StructLayout &lay) {
    const std::string fn_name = "__ovl_extent_" + lay.name;
    if (generated_overlay_resolvers_.count(fn_name)) return fn_name; // dedup
    generated_overlay_resolvers_.insert(fn_name);

    // Salvar contexto (mismo protocolo que generate_overlay_resolver).
    /* El guarda se lleva el contexto del padre y lo devuelve al salir. */
    ChildFunctionScope parent(*this);
    const bool saved_sret_active = sret_active_;
    const ir::IrValueId saved_sret_retbuf = sret_retbuf_;
    const uint64_t saved_sret_buf_size = sret_buf_size_;
    const bool saved_returns_fn = current_fn_returns_function_;
    sret_active_ = false;
    sret_retbuf_ = ir::IR_NO_VALUE;
    sret_buf_size_ = 0;
    current_fn_returns_function_ = false;

    ir::IrFunction child_fn;
    child_fn.name = fn_name;
    child_fn.ret_type = ir::IrType::U64; // el span en bytes
    ir::IrValueId self_pv = child_fn.new_value(ir::IrType::PTR, "%self");
    child_fn.values[self_pv].is_param = true;
    child_fn.values[self_pv].is_host_ptr = true;
    child_fn.params.push_back(self_pv);

    const ir::IrBlockId entry = child_fn.new_block("entry");
    fn_ = &child_fn;
    current_block_ = entry;
    push_scope();

    bind("base", self_pv);
    bind("this", self_pv);
    // Ligar hermanos escalares de offset constante (para
    // offset_expr/count/stride).
    for (const auto &sib : lay.fields) {
        if (sib.offset_expr || sib.offset_block || sib.array_count ||
            sib.array_stride || sib.element_block)
            continue;
        ir::IrValueId saddr = self_pv;
        if (sib.offset != 0) {
            ir::IrValueId so =
                emit_const(ir::IrType::I64, (uint64_t)sib.offset, 0);
            saddr = child_fn.new_value(ir::IrType::PTR);
            child_fn.values[saddr].is_host_ptr = true;
            ir::IrInstr a{};
            a.op = ir::IrOp::ADD;
            a.type = ir::IrType::PTR;
            a.dst = saddr;
            a.operands = {self_pv, so};
            child_fn.append(entry, std::move(a));
        }
        const ir::IrType st = ir_type_from_primitive(sib.type.kind);
        ir::IrValueId sv = child_fn.new_value(st);
        ir::IrInstr l{};
        l.op = ir::IrOp::LOAD;
        l.type = st;
        l.dst = sv;
        l.operands = {saddr};
        child_fn.append(entry, std::move(l));
        bind(sib.name, sv);
    }

    // Helpers de emision (u64).
    auto bin = [&](ir::IrOp op, ir::IrValueId a, ir::IrValueId b,
                   ir::IrType t) {
        return emit_ir_binop(op, a, b, t, 0);
    };
    // max sin ramas: max(a,b) = b ^ ((a^b) & -(a>b)).
    auto emit_max = [&](ir::IrValueId a, ir::IrValueId b) -> ir::IrValueId {
        ir::IrValueId gt = bin(ir::IrOp::CMP_UGT, a, b, ir::IrType::BOOL);
        ir::IrValueId mask = fn_->new_value(ir::IrType::U64);
        {
            ir::IrInstr n{};
            n.op = ir::IrOp::NEG;
            n.type = ir::IrType::U64;
            n.dst = mask;
            n.operands = {gt};
            emit(current_block_, std::move(n));
        }
        ir::IrValueId axb = bin(ir::IrOp::XOR, a, b, ir::IrType::U64);
        ir::IrValueId tmp = bin(ir::IrOp::AND, axb, mask, ir::IrType::U64);
        return bin(ir::IrOp::XOR, b, tmp, ir::IrType::U64);
    };

    ir::IrValueId maxv = emit_const(ir::IrType::U64, 0, 0);
    for (const auto &fi : lay.fields) {
        // Saltar lo que extent no cubre (documentado).
        if (fi.resolver_uses_parent) continue; // necesita root
        if (fi.is_array && (!fi.array_count || fi.element_block))
            continue; // sin count / @element
        ir::IrValueId end = ir::IR_NO_VALUE;
        const uint64_t fsz = fi.size;
        // base_off del campo/array (relativo a self).
        ir::IrValueId base_off;
        if (fi.offset_block) {
            const std::string rname = generate_overlay_resolver(lay, fi);
            ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
            fn_->values[addr].is_host_ptr = true;
            ir::IrInstr in{};
            in.op = ir::IrOp::CALL;
            in.func_name = rname;
            in.type = ir::IrType::PTR;
            in.dst = addr;
            in.operands = {self_pv};
            in.is_call_site = true;
            emit(current_block_, std::move(in));
            base_off = bin(ir::IrOp::SUB, addr, self_pv, ir::IrType::U64);
        } else if (fi.offset_expr) {
            base_off = lower_expr(fi.offset_expr);
            if (base_off == ir::IR_NO_VALUE) continue;
        } else {
            base_off = emit_const(ir::IrType::U64, (uint64_t)fi.offset, 0);
        }
        if (fi.is_array) {
            ir::IrValueId cnt = lower_expr(fi.array_count);
            ir::IrValueId strd = lower_expr(fi.array_stride);
            if (cnt == ir::IR_NO_VALUE || strd == ir::IR_NO_VALUE) continue;
            ir::IrValueId span = bin(ir::IrOp::MUL, cnt, strd, ir::IrType::U64);
            end = bin(ir::IrOp::ADD, base_off, span, ir::IrType::U64);
        } else {
            ir::IrValueId sz = emit_const(ir::IrType::U64, fsz, 0);
            end = bin(ir::IrOp::ADD, base_off, sz, ir::IrType::U64);
        }
        maxv = emit_max(maxv, end);
    }
    {
        ir::IrInstr rt{};
        rt.op = ir::IrOp::RET;
        rt.type = ir::IrType::U64;
        rt.operands = {maxv};
        emit(current_block_, std::move(rt));
        block_terminated_ = true;
    }

    pop_scope();
    pending_spawn_helpers_.push_back(std::move(child_fn));

    sret_active_ = saved_sret_active;
    sret_retbuf_ = saved_sret_retbuf;
    sret_buf_size_ = saved_sret_buf_size;
    current_fn_returns_function_ = saved_returns_fn;
    return fn_name;
}

ir::IrValueId Lowering::lower_field_addr(ast::FieldAccessExpr *e) {
    // `lib.G` sobre un global de otro modulo: no hay base que bajar (`lib` es
    // un namespace, no un valor), la direccion ES la del slot compartido.
    // Punto unico: por aqui pasan la lectura, la escritura y el `&`.
    uint64_t ns_slot = 0;
    if (imported_global_slot_of(e, ns_slot)) {
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        // El storage vive en memoria host (seccion `gdata`), como el de
        // cualquier global.
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr is{};
        is.op = ir::IrOp::STR_LIT_ADDR;
        is.type = ir::IrType::PTR;
        is.dst = v;
        is.imm = ns_slot;
        is.source_line = e->loc.line;
        emit(current_block_, std::move(is));
        return v;
    }
    const ir::IrValueId base = lower_expr(e->base.get());
    if (base == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

    const Type bt = e->base->result_type;
    if (bt.kind != PrimitiveKind::STRUCT) {
        error_at(e->loc, "lowering: '.' sobre tipo no-struct");
        return ir::IR_NO_VALUE;
    }
    const auto &layouts = tc_.struct_layouts();
    auto it = layouts.find(bt.struct_name);
    if (it == layouts.end()) {
        error_at(e->loc, "lowering: layout no disponible para struct '" +
                             bt.struct_name + "'");
        return ir::IR_NO_VALUE;
    }
    const StructLayout &lay = it->second;
    uint32_t offset = 0;
    const StructFieldInfo *fifound = nullptr;
    for (const auto &f : lay.fields) {
        if (f.name == e->field_name) {
            offset = f.offset;
            fifound = &f;
            break;
        }
    }
    // Campo `comptime` (property_kind=97): su slot vive apilado tras los campos
    // runtime (offset asignado en el layout, dentro de @c comptime_size_bytes).
    // Solo se accede desde codigo comptime (ctor/metodo comptime, ejecutado en
    // la ComptimeVM cuyo buffer `this` se dimensiona a @c comptime_size_bytes).
    // No hay overlay/bitfield/offset dinamico en campos comptime: address plana
    // base + offset.
    if (!fifound) {
        for (const auto &f : lay.comptime_fields) {
            if (f.name == e->field_name) {
                offset = f.offset;
                fifound = &f;
                break;
            }
        }
        if (fifound) {
            if (offset == 0) return base;
            const ir::IrValueId off_c =
                emit_const(ir::IrType::I64, offset, e->loc.line);
            const ir::IrValueId ca = fn_->new_value(ir::IrType::PTR);
            fn_->values[ca].is_host_ptr = fn_->values[base].is_host_ptr;
            ir::IrInstr ci{};
            ci.op = ir::IrOp::ADD;
            ci.type = ir::IrType::PTR;
            ci.dst = ca;
            ci.operands = {base, off_c};
            ci.source_line = e->loc.line;
            emit(current_block_, std::move(ci));
            return ca;
        }
    }
    if (!fifound) {
        error_at(e->loc, "lowering: campo '" + e->field_name +
                             "' no encontrado en struct '" + bt.struct_name +
                             "'");
        return ir::IR_NO_VALUE;
    }
    // Overlay F3: resolver de BLOQUE `@offset { ...; return <direccion>; }`.
    // Se sintetiza como funcion `__ovl_resolve_<S>_<f>(self)` (control de flujo
    // completo: if/else, multiples return; sin ALLOCA-en-bucle) y se llama con
    // el puntero base.  El resultado ES la direccion (host) donde
    // leer/escribir.
    if (fifound->offset_block) {
        const std::string rname = generate_overlay_resolver(lay, *fifound);
        ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
        fn_->values[addr].is_host_ptr = fn_->values[base].is_host_ptr;
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALL;
        ins.func_name = rname;
        ins.type = ir::IrType::PTR;
        ins.dst = addr;
        ins.operands = {base};
        // F4: enhebrar `root` si el resolver usa parent<T>() (la vista raiz de
        // la cadena de accesos: `pe` en `pe.Imports[i].name`).
        if (fifound->resolver_uses_parent) {
            const ir::IrValueId root_v = lower_overlay_root(e->base.get());
            if (root_v != ir::IR_NO_VALUE) ins.operands.push_back(root_v);
        }
        ins.is_call_site = true;
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        return addr;
    }

    // Overlay F2: offset DINAMICO `@offset(hermano + N)`.  Evaluamos la
    // expresion en tiempo de acceso; los nombres desnudos de campos hermanos
    // se enlazan a `LOAD [base + hermano.offset]` (host) en un scope temporal
    // y `lower_expr` los resuelve por @c lookup.  El DAG del type checker
    // garantiza que no hay ciclos.  fld_addr = base + offset_dinamico.
    if (fifound->offset_expr) {
        push_scope();
        for (const auto &sib : lay.fields) {
            if (sib.offset_expr) continue; // solo hermanos de offset constante
            ir::IrValueId saddr = base;
            if (sib.offset != 0) {
                ir::IrValueId so = emit_const(
                    ir::IrType::I64, (uint64_t)sib.offset, e->loc.line);
                saddr = fn_->new_value(ir::IrType::PTR);
                fn_->values[saddr].is_host_ptr = fn_->values[base].is_host_ptr;
                ir::IrInstr a{};
                a.op = ir::IrOp::ADD;
                a.type = ir::IrType::PTR;
                a.dst = saddr;
                a.operands = {base, so};
                a.source_line = e->loc.line;
                emit(current_block_, std::move(a));
            }
            const ir::IrType st = ir_type_from_primitive(sib.type.kind);
            ir::IrValueId sv = fn_->new_value(st);
            ir::IrInstr l{};
            l.op = ir::IrOp::LOAD;
            l.type = st;
            l.dst = sv;
            l.operands = {saddr};
            l.source_line = e->loc.line;
            emit(current_block_, std::move(l));
            bind(sib.name, sv);
        }
        const ir::IrValueId off_val = lower_expr(fifound->offset_expr);
        pop_scope();
        if (off_val == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrValueId fld_addr = fn_->new_value(ir::IrType::PTR);
        fn_->values[fld_addr].is_host_ptr = fn_->values[base].is_host_ptr;
        ir::IrInstr ins{};
        ins.op = ir::IrOp::ADD;
        ins.type = ir::IrType::PTR;
        ins.dst = fld_addr;
        ins.operands = {base, off_val};
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        return fld_addr;
    }

    if (offset == 0) return base;

    // ptr_field = ptr_base + offset.  Tratamos los punteros como i64
    // a efectos aritmeticos (la VM no distingue tipos de puntero a
    // este nivel; la aritmetica ya escalada queda en el caller).
    const ir::IrValueId off_val =
        emit_const(ir::IrType::I64, offset, e->loc.line);
    const ir::IrValueId fld_addr = fn_->new_value(ir::IrType::PTR);
    // B1 fix: heredar is_host_ptr del base.  Sin esto, el LOAD/STORE
    // posterior sobre fld_addr emite `mov` (memoria VM) cuando el base
    // es host_ptr -> lee/escribe garbage.  Caso observado:
    // `(*ptr_of(unique_struct)).y` con offset=4 leia 0 (memoria VM
    // aleatoria) en lugar del valor real del campo.
    fn_->values[fld_addr].is_host_ptr = fn_->values[base].is_host_ptr;
    ir::IrInstr ins{};
    ins.op = ir::IrOp::ADD;
    ins.type = ir::IrType::PTR;
    ins.dst = fld_addr;
    ins.operands = {base, off_val};
    ins.source_line = e->loc.line;
    emit(current_block_, std::move(ins));
    return fld_addr;
}

ir::IrValueId Lowering::emit_overlay_endian_swap(ast::Expr *base_expr,
                                                 const StructLayout &lay,
                                                 const StructFieldInfo &fi,
                                                 ir::IrValueId value,
                                                 uint32_t line) {
    if (!fi.endian_expr) return value;
    // 1. Evaluar la expr de endianness con los hermanos ligados desde la base.
    const ir::IrValueId ov_base = lower_expr(base_expr);
    if (ov_base == ir::IR_NO_VALUE) return value;
    push_scope();
    bind("base", ov_base);
    bind("this", ov_base);
    for (const auto &sib : lay.fields) {
        if (sib.offset_expr || sib.offset_block || sib.array_count ||
            sib.array_stride || sib.element_block)
            continue; // solo escalares de offset constante
        ir::IrValueId saddr = ov_base;
        if (sib.offset != 0) {
            ir::IrValueId so =
                emit_const(ir::IrType::I64, (uint64_t)sib.offset, line);
            saddr = fn_->new_value(ir::IrType::PTR);
            fn_->values[saddr].is_host_ptr = fn_->values[ov_base].is_host_ptr;
            ir::IrInstr a{};
            a.op = ir::IrOp::ADD;
            a.type = ir::IrType::PTR;
            a.dst = saddr;
            a.operands = {ov_base, so};
            a.source_line = line;
            emit(current_block_, std::move(a));
        }
        const ir::IrType st = ir_type_from_primitive(sib.type.kind);
        ir::IrValueId sv = fn_->new_value(st);
        ir::IrInstr l{};
        l.op = ir::IrOp::LOAD;
        l.type = st;
        l.dst = sv;
        l.operands = {saddr};
        l.source_line = line;
        emit(current_block_, std::move(l));
        bind(sib.name, sv);
    }
    const ir::IrValueId big = lower_expr(fi.endian_expr);
    pop_scope();
    if (big == ir::IR_NO_VALUE) return value;

    auto bin = [&](ir::IrOp op, ir::IrValueId a, ir::IrValueId b) {
        return emit_ir_binop(op, a, b, ir::IrType::U64, line);
    };
    // 2. sw = bswap64(value) >> (8-w)*8  (BYTESWAP swapea los 8 bytes).
    ir::IrValueId sw64 = fn_->new_value(ir::IrType::U64);
    {
        ir::IrInstr b{};
        b.op = ir::IrOp::BYTESWAP;
        b.type = ir::IrType::U64;
        b.dst = sw64;
        b.operands = {value};
        b.source_line = line;
        emit(current_block_, std::move(b));
    }
    ir::IrValueId sw = sw64;
    if (fi.size < 8) {
        ir::IrValueId shamt =
            emit_const(ir::IrType::U64, (uint64_t)(8 - fi.size) * 8, line);
        sw = bin(ir::IrOp::SHR, sw64, shamt);
    }
    // 3. select sin ramas: big ? sw : value = value ^ ((value ^ sw) &
    // -(big!=0)).
    ir::IrValueId zero = emit_const(ir::IrType::U64, 0, line);
    ir::IrValueId nz =
        emit_ir_binop(ir::IrOp::CMP_NE, big, zero, ir::IrType::BOOL, line);
    ir::IrValueId mask = fn_->new_value(ir::IrType::U64);
    {
        ir::IrInstr n{};
        n.op = ir::IrOp::NEG;
        n.type = ir::IrType::U64;
        n.dst = mask;
        n.operands = {nz};
        n.source_line = line;
        emit(current_block_, std::move(n));
    }
    ir::IrValueId vxs = bin(ir::IrOp::XOR, value, sw);
    ir::IrValueId tmp = bin(ir::IrOp::AND, vxs, mask);
    return bin(ir::IrOp::XOR, value, tmp);
}

ir::IrValueId Lowering::lower_class_field_load(ast::FieldAccessExpr *e) {
    // Limitacion G (cerrada): @c property_kind == 3 marca acceso a
    // static field via @c ClassName.field.  El base es IdentExpr cuyo
    // nombre es la clase; lo resolvemos via findclass inline + getstatic
    // con offset compile-time.  No leemos el tipo del base con
    // @c check_expr (fallaria por "nombre no declarado") sino que
    // tomamos el ClassLayout directamente del nombre.
    if (e->property_kind == 3) {
        if (!e->base || e->base->kind != ast::NodeKind::IdentExpr) {
            error_at(e->loc, "lowering: static field con base no-ClassName");
            return ir::IR_NO_VALUE;
        }
        auto *base_id = static_cast<ast::IdentExpr *>(e->base.get());
        auto it_cls = tc_.class_layouts().find(base_id->name);
        if (it_cls == tc_.class_layouts().end()) {
            error_at(e->loc,
                     "lowering: clase desconocida '" + base_id->name + "'");
            return ir::IR_NO_VALUE;
        }
        const ClassLayout &lay_s = it_cls->second;
        uint32_t s_off = 0;
        Type s_typ = Type{PrimitiveKind::COUNT};
        bool s_ok = false;
        for (const auto &f : lay_s.static_fields) {
            if (f.name == e->field_name) {
                s_off = f.offset;
                s_typ = f.type;
                s_ok = true;
                break;
            }
        }
        if (!s_ok) {
            error_at(e->loc, "lowering: static field '" + e->field_name +
                                 "' no encontrado en la clase '" +
                                 base_id->name + "'");
            return ir::IR_NO_VALUE;
        }
        // AOT (native_poo_): un campo estatico es almacenamiento por-clase,
        // sin ClassRegistry.  Lo mapeamos a un GLOBAL plano (slot static_data
        // unico por <Clase>_<campo>) -> STR_LIT_ADDR + LOAD, igual que un
        // global runtime.  Evita findclass+getstatic (runtime, no bare).
        if (native_poo_) {
            const ir::IrType ir_t = ir_type_from_primitive(s_typ.kind);
            const uint64_t slot = get_or_create_runtime_global_slot(
                "__static_" + base_id->name + "_" + e->field_name, 8);
            ir::IrValueId v_addr = emit_str_lit_addr(slot, e->loc.line, true);
            ir::IrValueId v_val = fn_->new_value(
                ir_t == ir::IrType::VOID ? ir::IrType::I64 : ir_t);
            {
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD;
                ld.type = (ir_t == ir::IrType::VOID) ? ir::IrType::I64 : ir_t;
                ld.dst = v_val;
                ld.operands = {v_addr};
                ld.source_line = e->loc.line;
                emit(current_block_, std::move(ld));
            }
            return v_val;
        }
        // 1) Sprint 5: findclass via IR ops (ALLOCA + STORE + FINDCLASS).
        const uint64_t cname_idx = intern_class_name(*out_mod_, base_id->name);
        const uint32_t cname_len = static_cast<uint32_t>(base_id->name.size());
        const ir::IrValueId v_cls =
            emit_findclass_by_name(cname_idx, cname_len, e->loc.line);
        // 2) getstatic {dst}, {src0}, offset_imm  -> v_val.
        // El opcode lee SIEMPRE 8 bytes (i64).  Para tipos < i64 la
        // semantica de sign/zero-extension coincide porque setstatic
        // almacena los bits high del reg fuente que el productor
        // sign-extendio (LOAD/CONST genericos hacen shl+sar para signed).
        const ir::IrType ir_t = ir_type_from_primitive(s_typ.kind);
        // Emite GETSTATIC IR op; el bytecode lee i64 que truncamos por tipo
        // mas abajo si el SSA val se usa como ancho menor (semantica heredada).
        ir::IrValueId v_val =
            emit_getstatic(v_cls, static_cast<uint64_t>(s_off), e->loc.line);
        // Cast al tipo logico del field si difiere de I64.
        if (ir_t != ir::IrType::I64) {
            v_val = cast_if_needed(v_val, ir::IrType::I64, ir_t, e->loc.line,
                                   /*is_explicit=*/true);
        }
        // Si el tipo del field es PTR host (no VirtualPtr), propagar
        // is_host_ptr al SSA value (mismo tratamiento que field de
        // instancia, ver final de esta funcion).
        // VirtualPtr (s_typ.is_virtual == true) NO recibe is_host_ptr.
        if (s_typ.kind == PrimitiveKind::PTR && !s_typ.is_virtual) {
            fn_->values[v_val].is_host_ptr = true;
        }
        return v_val;
    }

    const Type bt = e->base->result_type;
    if (bt.kind != PrimitiveKind::CLASS) {
        error_at(e->loc,
                 "lowering: '.' sobre tipo no-clase en lower_class_field_load");
        return ir::IR_NO_VALUE;
    }
    auto it = tc_.class_layouts().find(bt.struct_name);
    if (it == tc_.class_layouts().end()) {
        error_at(e->loc,
                 "lowering: clase desconocida '" + bt.struct_name + "'");
        return ir::IR_NO_VALUE;
    }
    const ClassLayout &lay = it->second;
    // si el type checker marco el acceso como propiedad, emitir
    // CALLVIRT al getter `get_<field_name>` en vez de getfield.
    if (e->property_kind == 1) {
        const std::string getter_name = std::string("get_") + e->field_name;
        const ClassMethodInfo *mtd = nullptr;
        for (const auto &m : lay.methods) {
            if (!m.is_constructor && m.name == getter_name) {
                mtd = &m;
                break;
            }
        }
        if (!mtd) {
            error_at(e->loc, "lowering: getter de propiedad '" + e->field_name +
                                 "' no encontrado en la clase '" +
                                 bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const ir::IrValueId obj = lower_expr(e->base.get());
        if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrType ret_ir = ir_type_from_primitive(mtd->return_type.kind);
        const ir::IrValueId dst = (ret_ir == ir::IrType::VOID)
                                      ? ir::IR_NO_VALUE
                                      : fn_->new_value(ret_ir);
        // Sprint edge-bugs (2026-06-03): si el metodo retorna CLASS, el
        // dst es un host_ptr a un objeto GC.  Marcarlo asi para que el
        // regalloc lo trate como GC-managed (save_live_regs lo convierte
        // a GcHandle antes de cualquier CALL siguiente).  Sin esto un
        // patron `p2 = p1.factory(); p3 = p2.factory();` rompe en interp:
        // el host_ptr de p2 queda stale tras el GC dentro del segundo
        // factory.  Mismo bug con `*->is_host_ptr` no marcado para
        // tipos PTR (e.g. `int* get_buf()`).
        if (dst != ir::IR_NO_VALUE) {
            const PrimitiveKind rk = mtd->return_type.kind;
            if (rk == PrimitiveKind::CLASS) {
                fn_->values[dst].is_host_ptr = true;
                fn_->values[dst].is_gc_object = true;
            } else if ((rk == PrimitiveKind::PTR ||
                        rk == PrimitiveKind::ARRAY) &&
                       !mtd->return_type.is_virtual) {
                fn_->values[dst].is_host_ptr = true;
            }
        }
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLVIRT;
        ins.type = ret_ir;
        ins.dst = dst;
        ins.operands.push_back(obj);
        ins.imm = static_cast<uint64_t>(mtd->vtable_index);
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        return dst;
    }
    uint32_t off = 0;
    bool ok = false;
    Type ftyp = Type{PrimitiveKind::COUNT};
    for (const auto &f : lay.fields) {
        if (f.name == e->field_name) {
            off = f.offset;
            ftyp = f.type;
            ok = true;
            break;
        }
    }
    if (!ok) {
        error_at(e->loc, "lowering: campo '" + e->field_name +
                             "' no encontrado en la clase '" + bt.struct_name +
                             "'");
        return ir::IR_NO_VALUE;
    }
    const ir::IrValueId obj = lower_expr(e->base.get());
    if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
    // Bajar a addr = obj + off (host_ptr) + LOAD estandar.  El emisor
    // IR consulta is_host_ptr y emite movh (memoria host).  Esto evita
    // el patron cur0/gcderef que colisionaba con el regalloc.
    const ir::IrValueId addr =
        emit_field_addr(fn_, current_block_, obj, off, e->loc.line);
    // Campo STRUCT: es INLINE en el payload de la clase (no un puntero).  Su
    // "valor" como agregado ES su DIRECCION (igual que un struct local: el SSA
    // value de un struct es su buffer).  NO hacer LOAD (eso leeria los primeros
    // bytes tratando el campo como puntero -> `obj.s.x` accederia a [[obj+off]]
    // en vez de [obj+off]; en AOT el campo sin inicializar es 0 -> store/deref
    // a NULL -> SIGSEGV).  Devolver la direccion del campo inline.  (Los arrays
    // T[] dinamicos SI son punteros host; los sized inline tienen su propio
    // manejo mas abajo, por eso solo interceptamos STRUCT aqui.)
    if (ftyp.kind == PrimitiveKind::STRUCT) {
        return addr;
    }
    // Campo shared<T> (H5): el campo ES el slot que guarda el host_ptr al ctrl.
    // Su "valor" como shared es la DIRECCION del campo (igual que un struct
    // inline): use_count/ptr_of cargan el ctrl desde [field_addr].  NO hacer
    // LOAD aqui (eso devolveria el ctrl ptr y ptr_of lo trataria como slot).
    if (ftyp.kind == PrimitiveKind::SHARED_PTR) {
        return addr;
    }
    const ir::IrType ir_t = ir_type_from_primitive(ftyp.kind);
    const ir::IrValueId dst = fn_->new_value(ir_t);
    ir::IrInstr ld{};
    ld.op = ir::IrOp::LOAD;
    ld.type = ir_t;
    ld.dst = dst;
    ld.operands = {addr};
    ld.source_line = e->loc.line;
    emit(current_block_, std::move(ld));
    // fix: si el TIPO del campo es PTR (host pointer obtenido
    // via malloc o similar), propagar @c is_host_ptr=true al SSA value
    // resultante para que indexaciones / derefs posteriores emitan
    // @c movh y NO @c mov (que iria a VM memory y leeria garbage).
    // Sin esto, `box.p[i]` con `box.p: i32*` cargaba con `mov` en lugar
    // de `movh`, leyendo memoria virtual VM en vez del buffer host
    // de malloc -> garbage o segfault.
    // EXCEPCION: VirtualPtr<T> (ftyp.is_virtual == true) es una direccion
    // VM aunque el tipo base sea PTR.  El valor cargado es una VA del
    // espacio VM, NO un puntero host.  Marcar is_host_ptr=true sobre un
    // VirtualPtr causaria que `*field` emitiera movh en vez de mov,
    // interpretando la VA como direccion host -> segfault.
    if (ftyp.kind == PrimitiveKind::PTR && !ftyp.is_virtual) {
        fn_->values[dst].is_host_ptr = true;
    }
    // Dynamic arrays `T[]` (size==0) stored as fields also hold host_ptrs
    // (from `new T[N]` via RAW_ALLOC).  Sin esto, `box.data[i] = ...`
    // emitia `mov` (VM mem) en vez de `movh` (host mem) tras LOAD del
    // field -> escribia/leia en vm_mem en una direccion que es realmente
    // host -> valores corrompidos.  Bug bug4-extension.
    // Para arrays dinamicos (`T[]`, array_size==0) el field guarda
    // un host_ptr (de `new T[N]` que usa RAW_ALLOC).  El default de
    // @c Type::make_array es is_virtual=true pero ese flag aplica a
    // arrays SIZED en stack; los dinamicos son siempre host.
    if (ftyp.kind == PrimitiveKind::ARRAY && ftyp.array_size == 0) {
        fn_->values[dst].is_host_ptr = true;
    }
    // Campo de tipo FUNCTION (lambda fn(...), NO cfn): en una CLASE el
    // campo guarda un PTR al slot heap de 16 bytes {fn_addr, env}
    // alocado con RAW_ALLOC (host) -- modelo de closures-en-campos
    // owned (RAII).  Marcar is_host_ptr=true para que, al llamar, las
    // cargas de fn_addr=[slot] y env=[slot+8] emitan movh (memoria
    // host) y NO mov (que iria a vm_mem y leeria basura -> callvmr a
    // una direccion invalida -> cuelgue/crash en VM/JIT).  En AOT todo
    // el espacio es host, por eso solo divergia en VM/JIT.
    // El cfn (fn_is_raw) es la direccion cruda de 8 bytes (CALLIND
    // directo, sin deref de slot), no necesita host-ness aqui.
    if (ftyp.kind == PrimitiveKind::FUNCTION && !ftyp.fn_is_raw) {
        fn_->values[dst].is_host_ptr = true;
    }
    // Campo unique<T> (ownership): el campo guarda la direccion del slot Tier 1
    // (16B) alocado en HEAP (RAW_ALLOC) cuando el unique va a un campo.  Marcar
    // el valor cargado como host_ptr para que ptr_of/read/use_count emitan movh
    // al deref-ear el slot (slot+0 ptr, slot+8 deleter); sin esto leerian
    // vm_mem en una direccion host -> 0/garbage.
    if (ftyp.kind == PrimitiveKind::UNIQUE_PTR) {
        fn_->values[dst].is_host_ptr = true;
    }
    // fix - field de tipo CLASS guarda un GcHandle (estable a
    // evacuacion del GC).  Tras LOADear el handle, hacemos @c gcderef
    // para obtener el host_ptr actual del objeto (refrescado tras
    // cualquier movimiento del GC).  Sin esta refresh, el ptr leido
    // del campo seria stale si el objeto migro a OldGen entre el store
    // y este load -> segfault al hacer @c callvirt o leer fields.
    if (ftyp.kind == PrimitiveKind::CLASS) {
        // raw_asm-elim 2026-05-28: gcderef + xchg -> IrOp::GC_DEREF_HOST.
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
        return v_host;
    }
    return dst;
}

ir::IrValueId Lowering::lower_class_field_store(ast::FieldAccessExpr *target,
                                                ir::IrValueId rhs,
                                                const SourceLoc &loc) {
    if (!target || rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
    // Limitacion G (cerrada): @c property_kind == 3 marca asignacion a
    // static field via @c ClassName.field = v.  Mismo patron que
    // lower_class_field_load: findclass inline + setstatic con offset
    // compile-time.  El base es IdentExpr (nombre de clase), no
    // referenciable como SSA value; resolvemos directo del layout.
    if (target->property_kind == 3) {
        if (!target->base || target->base->kind != ast::NodeKind::IdentExpr) {
            error_at(loc, "lowering: static field store con base no-ClassName");
            return ir::IR_NO_VALUE;
        }
        auto *base_id = static_cast<ast::IdentExpr *>(target->base.get());
        auto it_cls = tc_.class_layouts().find(base_id->name);
        if (it_cls == tc_.class_layouts().end()) {
            error_at(loc,
                     "lowering: clase desconocida '" + base_id->name + "'");
            return ir::IR_NO_VALUE;
        }
        const ClassLayout &lay_s = it_cls->second;
        uint32_t s_off = 0;
        Type s_typ = Type{PrimitiveKind::COUNT};
        bool s_ok = false;
        for (const auto &f : lay_s.static_fields) {
            if (f.name == target->field_name) {
                s_off = f.offset;
                s_typ = f.type;
                s_ok = true;
                break;
            }
        }
        if (!s_ok) {
            error_at(loc, "lowering: static field '" + target->field_name +
                              "' no encontrado en la clase '" + base_id->name +
                              "'");
            return ir::IR_NO_VALUE;
        }
        // Coerce rhs al tipo del field si difieren.
        const ir::IrType field_ir = ir_type_from_primitive(s_typ.kind);
        const ir::IrValueId rhs_cast =
            cast_if_needed(rhs, fn_->values[rhs].type, field_ir, loc.line);
        // AOT (native_poo_): campo estatico = global plano -> STR_LIT_ADDR +
        // STORE (mismo slot que la lectura: <Clase>_<campo>).
        if (native_poo_) {
            const uint64_t slot = get_or_create_runtime_global_slot(
                "__static_" + base_id->name + "_" + target->field_name, 8);
            ir::IrValueId v_addr = emit_str_lit_addr(slot, loc.line, true);
            emit_store_typed(v_addr, rhs_cast, field_ir, loc.line);
            return rhs_cast;
        }
        // 1) Sprint 5: findclass via IR ops.
        const uint64_t cname_idx = intern_class_name(*out_mod_, base_id->name);
        const uint32_t cname_len = static_cast<uint32_t>(base_id->name.size());
        const ir::IrValueId v_cls =
            emit_findclass_by_name(cname_idx, cname_len, loc.line);
        // 2) setstatic.  Coerce rhs_cast a I64 si fuera necesario.
        ir::IrValueId v_val_i64 = rhs_cast;
        if (fn_->values[rhs_cast].type != ir::IrType::I64) {
            v_val_i64 = cast_if_needed(rhs_cast, fn_->values[rhs_cast].type,
                                       ir::IrType::I64, loc.line,
                                       /*is_explicit=*/true);
        }
        emit_setstatic(v_cls, v_val_i64, static_cast<uint64_t>(s_off),
                       loc.line);
        return rhs_cast;
    }

    const Type bt = target->base->result_type;
    if (bt.kind != PrimitiveKind::CLASS) {
        error_at(
            loc,
            "lowering: '.' sobre tipo no-clase en lower_class_field_store");
        return ir::IR_NO_VALUE;
    }
    auto it = tc_.class_layouts().find(bt.struct_name);
    if (it == tc_.class_layouts().end()) {
        error_at(loc, "lowering: clase desconocida '" + bt.struct_name + "'");
        return ir::IR_NO_VALUE;
    }
    const ClassLayout &lay = it->second;
    // si el type checker marco el target como setter de
    // propiedad, emitir CALLVIRT al setter `set_<field_name>` en vez
    // de setfield.  El rhs se pasa como argumento del setter.
    if (target->property_kind == 2) {
        const std::string setter_name =
            std::string("set_") + target->field_name;
        const ClassMethodInfo *mtd = nullptr;
        for (const auto &m : lay.methods) {
            if (!m.is_constructor && m.name == setter_name) {
                mtd = &m;
                break;
            }
        }
        if (!mtd) {
            error_at(loc, "lowering: setter de propiedad '" +
                              target->field_name +
                              "' no encontrado en la clase '" + bt.struct_name +
                              "'");
            return ir::IR_NO_VALUE;
        }
        const ir::IrValueId obj = lower_expr(target->base.get());
        if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrType param_ir =
            mtd->param_types.empty()
                ? ir::IrType::I64
                : ir_type_from_primitive(mtd->param_types.front().kind);
        const ir::IrValueId rhs_cast =
            cast_if_needed(rhs, fn_->values[rhs].type, param_ir, loc.line);
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLVIRT;
        ins.type = ir::IrType::VOID;
        ins.dst = ir::IR_NO_VALUE;
        ins.operands.push_back(obj);
        ins.operands.push_back(rhs_cast);
        ins.imm = static_cast<uint64_t>(mtd->vtable_index);
        ins.source_line = loc.line;
        emit(current_block_, std::move(ins));
        return rhs_cast;
    }
    uint32_t off = 0;
    bool ok = false;
    Type ftyp = Type{PrimitiveKind::COUNT};
    for (const auto &f : lay.fields) {
        if (f.name == target->field_name) {
            off = f.offset;
            ftyp = f.type;
            ok = true;
            break;
        }
    }
    if (!ok) {
        error_at(loc, "lowering: campo '" + target->field_name +
                          "' no encontrado en la clase '" + bt.struct_name +
                          "'");
        return ir::IR_NO_VALUE;
    }
    const ir::IrValueId obj = lower_expr(target->base.get());
    if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
    // Reasignar un campo LAMBDA: liberar el slot+env (RAW_ALLOC owned) anterior
    // ANTES de guardar el nuevo, como reasignar un unique<T>.  Sin esto el
    // slot/env viejos se fugarian.  Null-guard interno (campo == 0 -> no libera
    // nada).  El nuevo slot+env (rhs) ya estan alocados y son distintos de los
    // viejos -> sin use-after-free.  Modelo sin GC -- ver
    // doc/VMdoc/Vesta/ClosuresEnCampos.md.
    if (ftyp.kind == PrimitiveKind::FUNCTION && !ftyp.fn_is_raw) {
        emit_free_closure_env_field(obj, off, loc.line);
    }
    const ir::IrValueId addr =
        emit_field_addr(fn_, current_block_, obj, off, loc.line);
    // Reasignar un campo unique<T>: capturamos el slot ANTERIOR (el campo aun
    // lo guarda) ANTES de sobreescribirlo; tras el store del nuevo lo liberamos
    // via CALL al helper __vx_free_uniq (NO inline, para no pegar el diamante
    // del free al tailcall del dtor en este call site -> evita el bucle).  El
    // nuevo slot ya se aloco -> distinto del viejo, sin double-free.
    ir::IrValueId uniq_old_slot = ir::IR_NO_VALUE;
    if (ftyp.kind == PrimitiveKind::UNIQUE_PTR) {
        uniq_old_slot = fn_->new_value(ir::IrType::I64);
        fn_->values[uniq_old_slot].is_host_ptr = true;
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = uniq_old_slot;
        ld.operands = {addr};
        ld.source_line = loc.line;
        emit(current_block_, std::move(ld));
    }
    // Campo STRUCT value-type (Fase 2b/3): el campo es un struct inline; @c rhs
    // es la DIRECCION del struct origen.  Copiamos memberwise (qword-by-qword)
    // sus bytes al campo -- NO un STORE escalar (que pisaria el primer qword
    // con la direccion origen).  move-on-store: el local origen ya esta en
    // @c escaping_locals_ (scan_escaping_locals marca el value de un store a
    // campo) -> su dtor de scope-exit se suprime; solo el dtor augmentado del
    // contenedor libera el recurso (un unico free).  Identico interp/JIT/AOT.
    if (ftyp.kind == PrimitiveKind::STRUCT) {
        uint64_t sz = 8;
        auto it_sl = tc_.struct_layouts().find(ftyp.struct_name);
        if (it_sl != tc_.struct_layouts().end())
            sz = static_cast<uint64_t>(it_sl->second.size_bytes);
        const bool dst_host = fn_->values[addr].is_host_ptr;
        const bool src_host = fn_->values[rhs].is_host_ptr;
        const uint64_t qwords = (sz + 7) / 8;
        for (uint64_t qi = 0; qi < qwords; ++qi) {
            const ir::IrValueId v_off = emit_const(
                ir::IrType::I64, static_cast<int64_t>(qi * 8), loc.line);
            const ir::IrValueId v_src_at = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_src_at].is_host_ptr = src_host;
            {
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = v_src_at;
                ad.operands = {rhs, v_off};
                ad.source_line = loc.line;
                emit(current_block_, std::move(ad));
            }
            const ir::IrValueId v_word =
                emit_load_typed(v_src_at, ir::IrType::I64, loc.line);
            const ir::IrValueId v_dst_at = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_dst_at].is_host_ptr = dst_host;
            {
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = v_dst_at;
                ad.operands = {addr, v_off};
                ad.source_line = loc.line;
                emit(current_block_, std::move(ad));
            }
            emit_store_typed(v_dst_at, v_word, ir::IrType::I64, loc.line);
        }
        // Copy-hook (ruta B): si el campo struct declara `__clone__`, este
        // store es una COPIA -> tras el memcpy, `campo.__clone__()` aplica el
        // efecto (p.ej. ++refcount) sobre la copia del campo.  El campo vive en
        // el payload HOST de la clase, asi que usamos el helper (copia a temp
        // VM en interp/JIT; el __clone__ opera sobre el pointee, sin
        // copy-back).  El origen NO se mueve (scan_escaping_locals lo excluye
        // para copy-hook).
        if (it_sl != tc_.struct_layouts().end() &&
            it_sl->second.has_copy_hook) {
            emit_struct_method_on_host_field(
                addr, ftyp.struct_name, ftyp.struct_name + "__" + "__clone__",
                loc.line);
        }
        return rhs;
    }
    // Campo shared<T> (H5): el campo guarda el host_ptr al bloque de control
    // (NO el slot stack del origen, que colgaria).  El store es una COPIA:
    //   1. dec del shared ANTERIOR del campo (free-when-0; no-op si era 0).
    //   2. LOAD ctrl desde [rhs] (rhs = slot del shared origen).
    //   3. STORE ctrl al campo.
    //   4. inc del refcount (el campo es un dueno mas).
    // El origen conserva su propia referencia (no se mueve;
    // scan_escaping_locals lo excluye).  El dtor del contenedor decrementa el
    // campo (dec-on-dtor).
    if (ftyp.kind == PrimitiveKind::SHARED_PTR) {
        // 1. dec del valor anterior del campo (reasignacion sin fuga).
        emit_shared_refcount_dec(addr, loc.line);
        // 2. LOAD ctrl desde el slot del origen.
        const ir::IrValueId v_ctrl = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_ctrl].is_host_ptr = true;
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_ctrl;
            ld.operands = {rhs};
            ld.source_line = loc.line;
            emit(current_block_, std::move(ld));
        }
        // 3. STORE ctrl al campo.
        emit_store_typed(addr, v_ctrl, ir::IrType::I64, loc.line);
        // 4. inc del refcount (el campo es un dueno mas).
        emit_shared_refcount_inc(addr, loc.line);
        return rhs;
    }
    const ir::IrType ir_t = ir_type_from_primitive(ftyp.kind);
    const ir::IrValueId rhs_cast =
        cast_if_needed(rhs, fn_->values[rhs].type, ir_t, loc.line);
    // Si el campo es CLASS, almacenamos el GcHandle (estable a evacuacion
    // del GC) en vez del host_ptr crudo.  Sin esto, una alocacion entre
    // el store y el siguiente load podria mover el objeto y dejar el ptr
    // guardado apuntando a memoria liberada/reusada -> segfault al
    // hacer @c callvirt sobre `this.field`.
    ir::IrValueId v_to_store = rhs_cast;
    if (ftyp.kind == PrimitiveKind::CLASS) {
        v_to_store = emit_gc_handle_for_ptr(rhs_cast, loc.line);
    }
    emit_store_typed(addr, v_to_store, ir_t, loc.line);
    // Write-barrier generacional old->young.  Al guardar una referencia GC
    // (campo CLASS) en el campo de un objeto que puede ser OLD, registrar el
    // CONTENEDOR en el remembered_set del GC para que el minor_gc encuentre el
    // young alcanzable SOLO via este campo old->young.  Sin el barrier, el
    // nursery preciso perderia ese young (UAF con el GC movible).  Se emite
    // TRAS el STORE del puntero young.  Solo en interp/JIT (comparten el GcHeap
    // con nursery real): en AOT (native_poo_) es NO-OP -- el nursery queda
    // vacio (alloc_pinned -> OldGen) y el major escanea preciso via field-maps
    // -> no se emite.  El contenedor `obj` es un host_ptr; GC_HANDLE_FOR_PTR lo
    // mapea a su GcHandle.  GCWB_IR baja a `gcwb` (interp) o a
    // vrt_gc_write_barrier (JIT); write_barrier() filtra por generacion (skip
    // si el contenedor es YOUNG) -> el remembered_set solo acumula old->young
    // reales.
    if (ftyp.kind == PrimitiveKind::CLASS && !native_poo_) {
        const ir::IrValueId v_cont_handle =
            emit_gc_handle_for_ptr(obj, loc.line);
        ir::IrInstr wb{};
        wb.op = ir::IrOp::GCWB_IR;
        wb.type = ir::IrType::VOID;
        wb.dst = ir::IR_NO_VALUE;
        wb.operands = {v_cont_handle};
        wb.source_line = loc.line;
        emit(current_block_, std::move(wb));
    }
    // Reassign-free del campo unique<T> via CALL al helper (1 instr, sin
    // diamante en el call site).  El helper hace null-guard internamente -> el
    // primer store (campo == 0) es un no-op.
    if (uniq_old_slot != ir::IR_NO_VALUE) {
        needs_free_uniq_helper_ = true;
        ir::IrInstr ci{};
        ci.op = ir::IrOp::CALL;
        ci.type = ir::IrType::VOID;
        ci.dst = ir::IR_NO_VALUE;
        ci.func_name = "__vx_free_uniq";
        ci.operands = {uniq_old_slot};
        ci.source_line = loc.line;
        ci.is_call_site = true;
        emit(current_block_, std::move(ci));
    }
    return rhs_cast;
}

} // namespace vx
