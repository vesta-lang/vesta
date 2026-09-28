/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file init_list_target.cpp
 * @brief Tipado de una lista `{...}` por el destino al que se guarda.
 *
 * Una lista de inicializacion no dice que construye: lo dice donde acaba.  La
 * declaracion (`Punto p = {...}`) y el literal compuesto (`(Punto){...}`) ya
 * lo sabian; una ASIGNACION o un `return` no, y la lista llegaba al lowering
 * sin tipo, que la rechazaba.  Este es el UNICO sitio que decide, para esos
 * destinos, que struct construye la lista.
 */

#include "vx/type_checker.h"

namespace vx {

void TypeChecker::type_init_list_from_target(ast::Expr *value,
                                             const Type &target) {
    if (!value || value->kind != ast::NodeKind::InitListExpr) return;
    auto *il = static_cast<ast::InitListExpr *>(value);
    // Un struct se anota aunque su layout aun no este: los valores de un
    // `enum X : Rgb { A = (Rgb){...} }` se comprueban antes.  Si al bajar no
    // hay struct con ese nombre, lo dice el lowering (VX3008).
    if (target.kind == PrimitiveKind::STRUCT) {
        il->target_type = target;
        return;
    }
    // Un array de tamano FIJO: la lista escribe sus elementos en el sitio.
    // Sin tamano (`T[]`) no hay hueco que rellenar -- es un puntero --.
    if (target.kind == PrimitiveKind::ARRAY && target.pointee &&
        target.array_size > 0) {
        il->target_type = target;
        il->target_type.is_const = false; // el destino se escribe
        // (Sus elementos toman el tipo de su hueco en check_init_list.)
    }
}

/**
 * @brief El CAMPO que rellena el elemento @p i de una lista hacia un struct
 *        (por nombre o por posicion).
 * @param il  La lista.
 * @param lay Layout del struct destino, o nulo si no es un struct.
 * @param i   Posicion del elemento.
 * @return La ficha del campo, o nulo si no se sabe.
 */
static const StructFieldInfo *init_list_slot_field(const ast::InitListExpr *il,
                                                   const StructLayout *lay,
                                                   size_t i) {
    if (!lay) return nullptr;
    if (il->is_designated)
        return i < il->field_names.size()
                   ? find_field(*lay, il->field_names[i])
                   : nullptr;
    return i < lay->fields.size() ? &lay->fields[i] : nullptr;
}

/**
 * @brief El tipo del hueco @p i de una lista ya tipada: el elemento de un
 *        array o el campo de un struct (por nombre o por posicion).
 * @param il  La lista.
 * @param lay Layout del struct destino, o nulo si no es un struct.
 * @param i   Posicion del elemento.
 * @return El tipo del hueco, o nulo si no se sabe.
 */
static const Type *init_list_slot_type(const ast::InitListExpr *il,
                                       const StructLayout *lay, size_t i) {
    const Type &target = il->target_type;
    if (target.kind == PrimitiveKind::ARRAY) return target.pointee.get();
    const StructFieldInfo *fi = init_list_slot_field(il, lay, i);
    return fi ? &fi->type : nullptr;
}

Type TypeChecker::check_init_list(ast::InitListExpr *il) {
    const StructLayout *lay = nullptr;
    if (il->target_type.kind == PrimitiveKind::STRUCT) {
        auto it = struct_layouts_.find(il->target_type.struct_name);
        if (it != struct_layouts_.end()) lay = &it->second;
    }
    for (size_t i = 0; i < il->elements.size(); ++i) {
        ast::Expr *el = il->elements[i].get();
        const Type *slot = init_list_slot_type(il, lay, i);
        /* Dar valor a un campo es USARLO: un `private` o un `protected` no se
         * rellena desde fuera del tipo.  Se construye con lo que el tipo
         * ofrece -- una factoria, un constructor, un `set` --. */
        if (const StructFieldInfo *fi = init_list_slot_field(il, lay, i))
            (void)check_member_visible(*fi, el->loc);
        // Cada elemento sabe a que hueco va ANTES de comprobarse, como en una
        // asignacion: una lista anidada toma su tipo de ahi y una lambda sin
        // tipos en sus parametros (`(v) => v + k`) toma su firma.
        if (slot && slot->kind == PrimitiveKind::FUNCTION &&
            el->kind == ast::NodeKind::LambdaExpr)
            propagate_fn_type_to_lambda(static_cast<ast::LambdaExpr *>(el),
                                        *slot);
        Type tx = slot ? check_value_for(el, *slot) : check_expr(el);
        // El nombre de una funcion suelto (`{doble}`) toma su tipo del hueco,
        // igual que en una asignacion o un argumento.
        if (slot) tx = maybe_promote_func_ref(el, *slot, tx);
        el->result_type = tx;
    }
    const Type &target = il->target_type;
    if (target.kind == PrimitiveKind::STRUCT) return target;
    // Sin anotar: tipo dependiente del contexto; la declaracion lo refina.
    if (target.kind != PrimitiveKind::ARRAY) return Type{PrimitiveKind::COUNT};
    const std::string array_name = written_type_name(target);
    if (il->is_designated) {
        const std::string first =
            il->field_names.empty() ? std::string() : il->field_names.front();
        diags_.diag(il->loc, DiagLevel::ERR, "VX2141", {array_name, first});
        return target;
    }
    if (il->elements.size() > target.array_size) {
        diags_.diag(il->loc, DiagLevel::ERR, "VX2142",
                    {array_name, std::to_string(il->elements.size()),
                     std::to_string(target.array_size)});
        return target;
    }
    const Type &elem = *target.pointee;
    for (size_t i = 0; i < il->elements.size(); ++i) {
        const Type &tx = il->elements[i]->result_type;
        if (tx.kind == PrimitiveKind::COUNT || types_assignable(elem, tx))
            continue;
        diags_.diag(il->elements[i]->loc, DiagLevel::ERR, "VX2143",
                    {std::to_string(i + 1), written_type_name(tx),
                     written_type_name(elem)});
    }
    return target;
}

} // namespace vx
