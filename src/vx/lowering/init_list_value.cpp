/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file init_list_value.cpp
 * @brief Lo que construye una lista `{...}`: un struct o un array.
 *
 * La declaracion `T x = {...}`, el literal compuesto `(T){...}`, la lista que
 * el comprobador tipo por su destino (`a[i] = {...}`, `a = {1, 2}`,
 * `return {...}`, un argumento) y el `unique<T> p = {...}` son lo mismo:
 * rellenar un hueco desde una lista.  Antes eran cuatro copias del bucle que
 * ya no coincidian -- una admitia campos de bits y las demas no, otra no ponia
 * a cero ni aplicaba los valores por defecto --.  Aqui hay una.
 */

#include "vx/lowering.h"
#include "vx/method_names.h"

namespace vx {

/**
 * @brief Si @p elem es un literal: su conversion al tipo del hueco no avisa,
 *        igual que `u8 x = 65`.
 * @param elem El elemento.
 * @return true si es un literal.
 */
static bool is_literal_elem(const ast::Expr *elem) {
    switch (elem->kind) {
    case ast::NodeKind::IntLitExpr:
    case ast::NodeKind::FloatLitExpr:
    case ast::NodeKind::BoolLitExpr:
    case ast::NodeKind::CharLitExpr:
    case ast::NodeKind::NullLitExpr: return true;
    default: return false;
    }
}

void Lowering::emit_init_slot(ir::IrValueId slot, const Type &t,
                              ast::Expr *elem, uint32_t line) {
    // Una lista ANIDADA se rellena ahi mismo, sin construir nada aparte: el
    // hueco del padre ya esta a cero.
    if (elem->kind == ast::NodeKind::InitListExpr) {
        auto *sub = static_cast<ast::InitListExpr *>(elem);
        if (t.kind == PrimitiveKind::STRUCT) {
            auto it = tc_.struct_layouts().find(t.struct_name);
            if (it == tc_.struct_layouts().end()) {
                error_at(elem->loc, "lowering: struct '" + t.struct_name +
                                        "' sin layout (init anidado)");
                return;
            }
            emit_struct_init_fields(slot, it->second, sub, line);
            return;
        }
        if (t.kind == PrimitiveKind::ARRAY && t.pointee && t.array_size > 0) {
            for (size_t i = 0; i < sub->elements.size(); ++i)
                emit_init_slot(
                    emit_ptr_add(slot, i * size_of_type(*t.pointee), line),
                    *t.pointee, sub->elements[i].get(), line);
            return;
        }
        diags_.diag(elem->loc, DiagLevel::ERR, "VX3008", {});
        return;
    }
    // Un literal en un hueco `string` se promueve a cadena, igual que en
    // `arr[i] = "lit"`: guardado tal cual seria la direccion del literal.
    ir::IrValueId v_val =
        (t.kind == PrimitiveKind::STRING &&
         elem->kind == ast::NodeKind::StringLitExpr)
            ? lower_string_literal_to_string_object(
                  static_cast<ast::StringLitExpr *>(elem))
            : lower_expr(elem);
    if (v_val == ir::IR_NO_VALUE) return;
    // Un AGREGADO inline (struct, array fijo, lambda) desde una expresion se
    // COPIA: su valor es su direccion y un STORE escalar la guardaria.
    if (is_inline_aggregate(t)) {
        emit_memberwise_copy(slot, v_val, size_of_type(t), line);
        if (t.kind == PrimitiveKind::STRUCT) {
            auto it = tc_.struct_layouts().find(t.struct_name);
            if (it != tc_.struct_layouts().end() && it->second.has_copy_hook)
                emit_struct_method_on_host_field(
                    slot, t.struct_name, copy_hook_symbol(t.struct_name), line);
        }
        return;
    }
    const ir::IrType ir_t = ir_type_from_primitive(t.kind);
    v_val = cast_if_needed(v_val, fn_->values[v_val].type, ir_t, line,
                           /*is_explicit=*/is_literal_elem(elem));
    emit_store_typed(slot, v_val, ir_t, line);
}

void Lowering::emit_struct_init_fields(ir::IrValueId base_addr,
                                       const StructLayout &lay,
                                       ast::InitListExpr *il, uint32_t line) {
    // Primero los valores por defecto; la lista sobrescribe los campos que
    // nombre (DSE limpia lo muerto).
    emit_struct_field_defaults(base_addr, lay, line);
    for (size_t i = 0; i < il->elements.size(); ++i) {
        const StructFieldInfo *fi = nullptr;
        if (il->is_designated) {
            const std::string &fname = il->field_names[i];
            fi = find_field(lay, fname);
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
        // `base + off` hereda la memoria de `base` (anfitrion / VM).
        const ir::IrValueId v_addr =
            emit_ptr_add(base_addr, (uint64_t)fi->offset, line);
        ast::Expr *elem = il->elements[i].get();
        if (fi->bit_width == 0) {
            emit_init_slot(v_addr, fi->type, elem, line);
            continue;
        }
        // Campo de bits: la palabra la comparte con otros campos, asi que se
        // cambian solo sus bits.
        ir::IrValueId v_val = lower_expr(elem);
        if (v_val == ir::IR_NO_VALUE) continue;
        v_val = cast_if_needed(v_val, fn_->values[v_val].type,
                               ir_type_from_primitive(fi->type.kind), line,
                               /*is_explicit=*/is_literal_elem(elem));
        emit_bit_field_store(v_addr, *fi, v_val, line);
    }
}

void Lowering::emit_struct_fill_from_init_list(ir::IrValueId addr,
                                               const StructLayout &lay,
                                               ast::InitListExpr *il,
                                               uint32_t line) {
    // A cero antes de nada: los campos que la lista no nombra -- y los bits
    // libres de una palabra de campos de bits -- no pueden quedar con lo que
    // hubiera en la memoria.
    emit_zero_fill(addr, (uint64_t)lay.size_bytes, line);
    // El vptr no es un campo que la lista pueda escribir: sin fijarlo aqui, un
    // struct polimorfico construido asi despachaba por una tabla nula.
    if (lay.is_polymorphic) emit_struct_vptr_init(addr, lay, line);
    emit_struct_init_fields(addr, lay, il, line);
}

ir::IrValueId Lowering::emit_struct_from_init_list(const StructLayout &lay,
                                                   ast::InitListExpr *il,
                                                   uint32_t line) {
    // Un agregado como cualquier otro -> host en los tres modos.
    const ir::IrValueId addr = stack_alloc_buf(
        (uint64_t)lay.size_bytes, line, /*host_memory=*/true);
    emit_struct_fill_from_init_list(addr, lay, il, line);
    return addr;
}

void Lowering::emit_array_fill_from_init_list(ir::IrValueId addr,
                                              const Type &arr,
                                              ast::InitListExpr *il,
                                              uint32_t line) {
    // Los huecos que la lista no nombra valen cero, no lo que hubiera.
    emit_zero_fill(addr, (uint64_t)size_of_type(arr), line);
    const Type &elem = *arr.pointee;
    const uint64_t esz = size_of_type(elem);
    for (size_t i = 0; i < il->elements.size() && i < arr.array_size; ++i)
        emit_init_slot(emit_ptr_add(addr, i * esz, line), elem,
                       il->elements[i].get(), line);
}

ir::IrValueId Lowering::emit_array_from_init_list(const Type &arr,
                                                  ast::InitListExpr *il,
                                                  uint32_t line) {
    const ir::IrValueId addr = stack_alloc_buf(
        (uint64_t)size_of_type(arr), line, /*host_memory=*/true);
    emit_array_fill_from_init_list(addr, arr, il, line);
    return addr;
}

ir::IrValueId Lowering::lower_init_list_value(ast::InitListExpr *il) {
    const Type &t = il->target_type;
    if (t.kind == PrimitiveKind::STRUCT) {
        auto it = tc_.struct_layouts().find(t.struct_name);
        if (it != tc_.struct_layouts().end())
            return emit_struct_from_init_list(it->second, il, il->loc.line);
    }
    if (t.kind == PrimitiveKind::ARRAY && t.pointee && t.array_size > 0)
        return emit_array_from_init_list(t, il, il->loc.line);
    diags_.diag(il->loc, DiagLevel::ERR, "VX3008", {});
    return ir::IR_NO_VALUE;
}

} // namespace vx
