/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file type_classify.cpp
 * @brief Implementacion del clasificador de tipos (Fase 0 interop C/ownership).
 *
 * Funciones puras (modulo el resolver de structs) sin estado global.  La
 * recursion sobre campos de struct lleva un limite de profundidad defensivo:
 * los structs Vex no pueden contenerse a si mismos por valor (tamano infinito,
 * el type checker ya lo rechazaria), pero acotamos por si el resolver
 * devolviera un ciclo por error.
 */

#include "vx/type_classify.h"

#include "vx/type_checker.h" // StructLayout, StructFieldInfo, ClassMethodInfo
#include "vx/types.h"        // Type, PrimitiveKind

namespace vx {

namespace {

/// Profundidad maxima de recursion sobre campos anidados de struct.  Mas que
/// suficiente para cualquier jerarquia real; corta ciclos patologicos.
constexpr int kMaxDepth = 64;

/// @return true si el struct @c lay declara un destructor `~Struct()`.
bool struct_has_destructor(const StructLayout &lay) noexcept {
    for (const auto &m : lay.methods)
        if (m.is_destructor) return true;
    return false;
}

bool is_c_representable_rec(const Type &t, const StructResolver &find_struct,
                           int depth);
bool is_managed_rec(const Type &t, const StructResolver &find_struct,
                    int depth);

bool is_c_representable_rec(const Type &t, const StructResolver &find_struct,
                           int depth) {
    if (depth > kMaxDepth) return false; // ciclo defensivo: conservador (no-C)
    switch (t.kind) {
    // Primitivos escalares: ABI C directo.
    case PrimitiveKind::VOID:
    case PrimitiveKind::BOOL:
    case PrimitiveKind::CHAR:
    case PrimitiveKind::I8:
    case PrimitiveKind::I16:
    case PrimitiveKind::I32:
    case PrimitiveKind::I64:
    case PrimitiveKind::U8:
    case PrimitiveKind::U16:
    case PrimitiveKind::U32:
    case PrimitiveKind::U64:
    case PrimitiveKind::F32:
    case PrimitiveKind::F64:
        return true;
    // Puntero: host (8 bytes, ABI C) si; VirtualPtr (direccion VM) no.  El
    // pointee no importa: C ve un puntero opaco.
    case PrimitiveKind::PTR:
        return !t.is_virtual;
    // Borrow = host_ptr T* de 8 bytes en runtime -> ABI C.
    case PrimitiveKind::BORROW:
    case PrimitiveKind::BORROW_MUT:
        return true;
    // Array C inline: representable si el elemento lo es.
    case PrimitiveKind::ARRAY:
        return t.pointee &&
               is_c_representable_rec(*t.pointee, find_struct, depth + 1);
    // Funcion: cfn (puntero crudo 8B) si; lambda (env gestionada) no.
    case PrimitiveKind::FUNCTION:
        return t.fn_is_raw;
    // Optional/Result: POD etiquetado en stack; representable si sus payloads
    // lo son.
    case PrimitiveKind::OPTIONAL:
        return t.pointee &&
               is_c_representable_rec(*t.pointee, find_struct, depth + 1);
    case PrimitiveKind::RESULT:
        return t.pointee && t.pointee2 &&
               is_c_representable_rec(*t.pointee, find_struct, depth + 1) &&
               is_c_representable_rec(*t.pointee2, find_struct, depth + 1);
    // Struct: C-compat si TODOS sus campos lo son Y no tiene `~Struct()`.
    case PrimitiveKind::STRUCT: {
        const StructLayout *lay = find_struct ? find_struct(t.struct_name)
                                              : nullptr;
        if (!lay) return false; // no resuelto -> conservador
        if (struct_has_destructor(*lay)) return false;
        for (const auto &f : lay->fields) {
            if (!is_c_representable_rec(f.type, find_struct, depth + 1))
                return false;
        }
        return true;
    }
    // Tipos de referencia / gestionados / sin equivalente C.
    case PrimitiveKind::CLASS:
    case PrimitiveKind::STRING:
    case PrimitiveKind::ARRAYLIST:
    case PrimitiveKind::HASHMAP:
    case PrimitiveKind::HASHSET:
    case PrimitiveKind::QUEUE:
    case PrimitiveKind::DEQUE:
    case PrimitiveKind::TREEMAP:
    case PrimitiveKind::TREESET:
    case PrimitiveKind::STACK:
    case PrimitiveKind::FUTURE:
    case PrimitiveKind::UNIQUE_PTR:
    case PrimitiveKind::SHARED_PTR:
    case PrimitiveKind::GC_PTR:
    case PrimitiveKind::TYPE_META:
    case PrimitiveKind::COUNT:
    default:
        return false;
    }
}

bool is_managed_rec(const Type &t, const StructResolver &find_struct,
                    int depth) {
    if (depth > kMaxDepth) return true; // ciclo defensivo: conservador (gest.)
    switch (t.kind) {
    // Lambda con env (a nivel de tipo no sabemos el conteo de capturas;
    // conservador: potencialmente owned).  cfn NO es gestionado.
    case PrimitiveKind::FUNCTION:
        return !t.fn_is_raw;
    // Tipos gestionados directamente.
    case PrimitiveKind::STRING:
    case PrimitiveKind::CLASS:
    case PrimitiveKind::ARRAYLIST:
    case PrimitiveKind::HASHMAP:
    case PrimitiveKind::HASHSET:
    case PrimitiveKind::QUEUE:
    case PrimitiveKind::DEQUE:
    case PrimitiveKind::TREEMAP:
    case PrimitiveKind::TREESET:
    case PrimitiveKind::STACK:
    case PrimitiveKind::FUTURE:
    case PrimitiveKind::UNIQUE_PTR:
    case PrimitiveKind::SHARED_PTR:
    case PrimitiveKind::GC_PTR:
        return true;
    // Array: gestionado si su elemento lo es.
    case PrimitiveKind::ARRAY:
        return t.pointee &&
               is_managed_rec(*t.pointee, find_struct, depth + 1);
    // Optional/Result: gestionado si algun payload lo es.
    case PrimitiveKind::OPTIONAL:
        return t.pointee &&
               is_managed_rec(*t.pointee, find_struct, depth + 1);
    case PrimitiveKind::RESULT:
        return (t.pointee &&
                is_managed_rec(*t.pointee, find_struct, depth + 1)) ||
               (t.pointee2 &&
                is_managed_rec(*t.pointee2, find_struct, depth + 1));
    // Struct: gestionado si tiene un destructor o algun campo gestionado.
    case PrimitiveKind::STRUCT: {
        const StructLayout *lay = find_struct ? find_struct(t.struct_name)
                                              : nullptr;
        if (!lay) return false; // no resuelto -> no gestionado (conservador
                                // hacia C-compat; el boundary-check de Fase 4
                                // rechazaria un struct desconocido de todos
                                // modos).
        if (struct_has_destructor(*lay)) return true;
        for (const auto &f : lay->fields) {
            if (is_managed_rec(f.type, find_struct, depth + 1)) return true;
        }
        return false;
    }
    // Punteros (host o virtual) y borrows NO poseen: el cleanup, si lo hay, es
    // del apuntado, no del puntero.  Primitivos escalares: nunca gestionados.
    case PrimitiveKind::PTR:
    case PrimitiveKind::BORROW:
    case PrimitiveKind::BORROW_MUT:
    case PrimitiveKind::VOID:
    case PrimitiveKind::BOOL:
    case PrimitiveKind::CHAR:
    case PrimitiveKind::I8:
    case PrimitiveKind::I16:
    case PrimitiveKind::I32:
    case PrimitiveKind::I64:
    case PrimitiveKind::U8:
    case PrimitiveKind::U16:
    case PrimitiveKind::U32:
    case PrimitiveKind::U64:
    case PrimitiveKind::F32:
    case PrimitiveKind::F64:
    case PrimitiveKind::TYPE_META:
    case PrimitiveKind::COUNT:
    default:
        return false;
    }
}

} // namespace

bool is_c_representable(const Type &t, const StructResolver &find_struct) {
    return is_c_representable_rec(t, find_struct, 0);
}

bool is_managed(const Type &t, const StructResolver &find_struct) {
    return is_managed_rec(t, find_struct, 0);
}

} // namespace vx
