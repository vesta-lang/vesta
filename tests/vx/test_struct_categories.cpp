/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file test_struct_categories.cpp
 * @brief Test de integracion de la Fase 1 (interop C): inferencia de la
 *        categoria de un struct (C-compat vs gestionado) a partir de sus
 *        campos, marcada en @c StructLayout y expuesta via los metodos
 *        @c TypeChecker::type_is_c_representable / @c type_is_managed.
 *
 * A diferencia de @c test_type_classify (clasificador puro con layouts
 * sinteticos), aqui se corre el TypeChecker REAL sobre un programa Vex y se
 * verifican (a) los flags cacheados @c cat_* y (b) los metodos query.
 */

#include "vx/lexer.h"
#include "vx/parser.h"
#include "vx/type_checker.h"
#include "vx/types.h"

#include <cstdio>
#include <string>

using vx::Diagnostics;
using vx::Lexer;
using vx::Parser;
using vx::PrimitiveKind;
using vx::TypeChecker;
using vx::Type;

static int g_pass = 0;
static int g_fail = 0;

#define CK(cond)                                                              \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_pass;                                                          \
        } else {                                                               \
            ++g_fail;                                                          \
            std::printf("FAIL linea %d: %s\n", __LINE__, #cond);               \
        }                                                                      \
    } while (0)

int main() {
    // Structs variados: C-compat (POD/cfn/ptr/anidado-C) vs gestionado
    // (fn capturador, string, anidado-gestionado).
    const std::string src = R"VX(
struct Vec2 { f64 x; f64 y; }
struct WithCfn { i64 n; cfn(i64) -> i64 cb; }
struct WithPtr { i64* p; u8 flags; }
struct Managed { i64 n; fn() -> i64 onTick; }
struct WithStr { string s; i64 k; }
struct NestedC { Vec2 v; i64 z; }
struct NestedM { Managed m; i64 z; }
i32 main() { return 0; }
)VX";

    Diagnostics diags;
    Lexer lx(src, "<test>", diags);
    Parser p(lx, diags);
    auto mod = p.parse_program();
    CK(mod != nullptr);
    CK(!diags.has_errors());
    if (!mod || diags.has_errors()) {
        std::printf("test_struct_categories: parse fallo (abortado)\n");
        return 1;
    }

    TypeChecker tc(*mod, diags);
    const bool ok = tc.run();
    CK(ok);

    const auto &sl = tc.struct_layouts();

    // Helper: verifica los flags cacheados de un struct.
    auto check_cached = [&](const char *name, bool exp_crep, bool exp_mgd) {
        auto it = sl.find(name);
        CK(it != sl.end());
        if (it == sl.end()) return;
        CK(it->second.cat_computed);
        CK(it->second.cat_c_representable == exp_crep);
        CK(it->second.cat_managed == exp_mgd);
    };
    // Helper: verifica los metodos query (fuente de verdad).
    auto check_query = [&](const char *name, bool exp_crep, bool exp_mgd) {
        const Type t{PrimitiveKind::STRUCT, std::string(name)};
        CK(tc.type_is_c_representable(t) == exp_crep);
        CK(tc.type_is_managed(t) == exp_mgd);
    };

    // C-compat: copiables, cruzan la frontera C.
    check_cached("Vec2", true, false);
    check_query("Vec2", true, false);
    check_cached("WithCfn", true, false); // cfn = puntero crudo C
    check_query("WithCfn", true, false);
    check_cached("WithPtr", true, false); // i64* host + POD
    check_query("WithPtr", true, false);
    check_cached("NestedC", true, false); // Vec2 anidado (C-compat)
    check_query("NestedC", true, false);

    // Gestionado: move-only + RAII, no cruzan por valor.
    check_cached("Managed", false, true); // fn capturador
    check_query("Managed", false, true);
    check_cached("WithStr", false, true); // string (GC)
    check_query("WithStr", false, true);
    check_cached("NestedM", false, true); // contiene Managed
    check_query("NestedM", false, true);

    // Los metodos query tambien clasifican primitivos / tipos directos.
    CK(tc.type_is_c_representable(Type{PrimitiveKind::I64}));
    CK(!tc.type_is_managed(Type{PrimitiveKind::I64}));
    CK(!tc.type_is_c_representable(Type{PrimitiveKind::STRING}));
    CK(tc.type_is_managed(Type{PrimitiveKind::STRING}));

    if (g_fail == 0)
        std::printf("test_struct_categories: %d checks OK\n", g_pass);
    else
        std::printf("test_struct_categories: %d/%d FALLARON\n", g_fail,
                    g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
