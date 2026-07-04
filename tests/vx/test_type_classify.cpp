/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file test_type_classify.cpp
 * @brief Tests del clasificador de tipos (Fase 0 interop C / ownership).
 *
 * Verifica @c is_c_representable y @c is_managed sobre tipos construidos a
 * mano + un resolver de structs con layouts sinteticos.  No necesita el
 * TypeChecker completo: el clasificador es una funcion pura + resolver.
 */

#include "vx/type_classify.h"
#include "vx/type_checker.h" // StructLayout, StructFieldInfo, ClassMethodInfo
#include "vx/types.h"        // Type, PrimitiveKind

#include <cstdio>
#include <map>
#include <string>

using vx::ClassMethodInfo;
using vx::is_c_representable;
using vx::is_managed;
using vx::PrimitiveKind;
using vx::StructFieldInfo;
using vx::StructLayout;
using vx::StructResolver;
using vx::Type;

static int g_checks = 0;
static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fail;                                                          \
            std::printf("FAIL linea %d: %s\n", __LINE__, #cond);               \
        }                                                                      \
    } while (0)

// --- helpers de construccion ---

static Type prim(PrimitiveKind k) { return Type{k}; }

static Type host_ptr(PrimitiveKind pointee = PrimitiveKind::U8) {
    return Type::make_ptr(Type{pointee}, /*virt=*/false);
}
static Type virtual_ptr(PrimitiveKind pointee = PrimitiveKind::U8) {
    return Type::make_ptr(Type{pointee}, /*virt=*/true);
}

static Type cfn_ty() {
    Type t;
    t.kind = PrimitiveKind::FUNCTION;
    t.fn_is_raw = true; // puntero a funcion crudo (C)
    t.pointee = std::make_shared<Type>(Type{PrimitiveKind::I64}); // retorno
    return t;
}
static Type fn_ty() {
    Type t;
    t.kind = PrimitiveKind::FUNCTION;
    t.fn_is_raw = false; // lambda gestionado
    t.pointee = std::make_shared<Type>(Type{PrimitiveKind::I64});
    return t;
}

static Type struct_ty(const std::string &name) {
    return Type{PrimitiveKind::STRUCT, name};
}

static StructFieldInfo field(const std::string &name, Type ty) {
    StructFieldInfo f;
    f.name = name;
    f.type = std::move(ty);
    f.offset = 0;
    f.size = 8;
    return f;
}

int main() {
    // Tabla de layouts sinteticos + resolver.
    std::map<std::string, StructLayout> layouts;
    StructResolver resolver = [&](const std::string &n) -> const StructLayout * {
        auto it = layouts.find(n);
        return it == layouts.end() ? nullptr : &it->second;
    };

    // --- 1. Primitivos: C-representables, no gestionados ---
    for (PrimitiveKind k :
         {PrimitiveKind::VOID, PrimitiveKind::BOOL, PrimitiveKind::CHAR,
          PrimitiveKind::I8, PrimitiveKind::I32, PrimitiveKind::I64,
          PrimitiveKind::U8, PrimitiveKind::U64, PrimitiveKind::F32,
          PrimitiveKind::F64}) {
        CHECK(is_c_representable(prim(k), resolver));
        CHECK(!is_managed(prim(k), resolver));
    }

    // --- 2. Punteros: host C-rep, virtual no; ninguno gestionado ---
    CHECK(is_c_representable(host_ptr(), resolver));
    CHECK(!is_managed(host_ptr(), resolver));
    CHECK(!is_c_representable(virtual_ptr(), resolver)); // VM-space, no C
    CHECK(!is_managed(virtual_ptr(), resolver));

    // --- 3. cfn vs fn ---
    CHECK(is_c_representable(cfn_ty(), resolver)); // puntero crudo C
    CHECK(!is_managed(cfn_ty(), resolver));
    CHECK(!is_c_representable(fn_ty(), resolver)); // env gestionada
    CHECK(is_managed(fn_ty(), resolver));          // posee env

    // --- 4. Tipos gestionados directos: no C-rep, si gestionados ---
    for (PrimitiveKind k :
         {PrimitiveKind::STRING, PrimitiveKind::CLASS, PrimitiveKind::ARRAYLIST,
          PrimitiveKind::HASHMAP, PrimitiveKind::FUTURE,
          PrimitiveKind::UNIQUE_PTR, PrimitiveKind::SHARED_PTR}) {
        Type t{k};
        if (k == PrimitiveKind::CLASS) t.struct_name = "Foo";
        CHECK(!is_c_representable(t, resolver));
        CHECK(is_managed(t, resolver));
    }

    // --- 5. Borrow: host_ptr en runtime -> C-rep, no gestionado ---
    {
        Type b{PrimitiveKind::BORROW};
        b.pointee = std::make_shared<Type>(Type{PrimitiveKind::I32});
        CHECK(is_c_representable(b, resolver));
        CHECK(!is_managed(b, resolver));
    }

    // --- 6. Struct C-compat (solo campos POD/ptr/cfn) ---
    {
        StructLayout L;
        L.name = "Vec2";
        L.fields.push_back(field("x", prim(PrimitiveKind::F64)));
        L.fields.push_back(field("y", prim(PrimitiveKind::F64)));
        L.fields.push_back(field("cb", cfn_ty()));
        L.fields.push_back(field("p", host_ptr()));
        layouts["Vec2"] = std::move(L);
        CHECK(is_c_representable(struct_ty("Vec2"), resolver));
        CHECK(!is_managed(struct_ty("Vec2"), resolver));
    }

    // --- 7. Struct gestionado (campo fn capturador) ---
    {
        StructLayout L;
        L.name = "Widget";
        L.fields.push_back(field("n", prim(PrimitiveKind::I64)));
        L.fields.push_back(field("onTick", fn_ty())); // managed
        layouts["Widget"] = std::move(L);
        CHECK(!is_c_representable(struct_ty("Widget"), resolver));
        CHECK(is_managed(struct_ty("Widget"), resolver));
    }

    // --- 8. Struct gestionado por destructor ~Struct() (campos todos POD) ---
    {
        StructLayout L;
        L.name = "Conn";
        L.fields.push_back(field("fd", prim(PrimitiveKind::I64)));
        ClassMethodInfo dtor;
        dtor.name = "~Conn";
        dtor.is_destructor = true;
        L.methods.push_back(dtor);
        layouts["Conn"] = std::move(L);
        CHECK(!is_c_representable(struct_ty("Conn"), resolver)); // tiene dtor
        CHECK(is_managed(struct_ty("Conn"), resolver));
    }

    // --- 9. Struct anidado: C-compat contiene C-compat -> C-compat ---
    {
        StructLayout Inner;
        Inner.name = "Inner";
        Inner.fields.push_back(field("a", prim(PrimitiveKind::I32)));
        layouts["Inner"] = std::move(Inner);

        StructLayout Outer;
        Outer.name = "OuterC";
        Outer.fields.push_back(field("i", struct_ty("Inner")));
        Outer.fields.push_back(field("z", prim(PrimitiveKind::U8)));
        layouts["OuterC"] = std::move(Outer);
        CHECK(is_c_representable(struct_ty("OuterC"), resolver));
        CHECK(!is_managed(struct_ty("OuterC"), resolver));
    }

    // --- 10. Struct anidado: contiene struct gestionado -> gestionado ---
    {
        StructLayout Outer;
        Outer.name = "OuterM";
        Outer.fields.push_back(field("w", struct_ty("Widget"))); // managed
        layouts["OuterM"] = std::move(Outer);
        CHECK(!is_c_representable(struct_ty("OuterM"), resolver));
        CHECK(is_managed(struct_ty("OuterM"), resolver));
    }

    // --- 11. Array: C-rep si el elemento lo es; gestionado si lo es ---
    {
        Type arr_pod = Type::make_array(prim(PrimitiveKind::I32), 4);
        CHECK(is_c_representable(arr_pod, resolver));
        CHECK(!is_managed(arr_pod, resolver));

        Type arr_str = Type::make_array(Type{PrimitiveKind::STRING}, 4);
        CHECK(!is_c_representable(arr_str, resolver));
        CHECK(is_managed(arr_str, resolver));
    }

    // --- 12. Optional/Result: por payload ---
    {
        Type opt_pod{PrimitiveKind::OPTIONAL};
        opt_pod.pointee = std::make_shared<Type>(Type{PrimitiveKind::I64});
        CHECK(is_c_representable(opt_pod, resolver));
        CHECK(!is_managed(opt_pod, resolver));

        Type opt_str{PrimitiveKind::OPTIONAL};
        opt_str.pointee = std::make_shared<Type>(Type{PrimitiveKind::STRING});
        CHECK(!is_c_representable(opt_str, resolver));
        CHECK(is_managed(opt_str, resolver));

        Type res{PrimitiveKind::RESULT};
        res.pointee = std::make_shared<Type>(Type{PrimitiveKind::I64});  // V
        res.pointee2 = std::make_shared<Type>(Type{PrimitiveKind::I64}); // E
        CHECK(is_c_representable(res, resolver));
        CHECK(!is_managed(res, resolver));
    }

    // --- 13. Struct no resuelto: conservador (no C-rep, no gestionado) ---
    {
        CHECK(!is_c_representable(struct_ty("Desconocido"), resolver));
        CHECK(!is_managed(struct_ty("Desconocido"), resolver));
    }

    if (g_fail == 0)
        std::printf("test_type_classify: %d checks OK\n", g_checks);
    else
        std::printf("test_type_classify: %d/%d FALLARON\n", g_fail, g_checks);
    return g_fail == 0 ? 0 : 1;
}
