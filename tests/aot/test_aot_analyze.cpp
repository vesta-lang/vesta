/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file tests/aot/test_aot_analyze.cpp
 * @brief Tests de  AOT.1 -- analisis is_pure_native (clasificacion +
 *        admisibilidad por target + reporte de incompatibilidades).
 */

#include "aot/aot_analyze.h"
#include "ir/ssa_ir.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace aot;
using ir::IrOp;
using ir::IrType;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fails;                                                         \
            std::printf("  FAIL linea %d: %s\n", __LINE__, #cond);             \
        }                                                                      \
    } while (0)

/// Construye una funcion IR con un unico bloque que contiene las ops dadas.
static ir::IrFunction make_fn(const std::string &name,
                              const std::vector<IrOp> &ops,
                              uint32_t base_line = 10) {
    ir::IrFunction fn;
    fn.name = name;
    fn.ret_type = IrType::VOID;
    ir::IrBlock blk;
    blk.id = 0;
    blk.name = "entry";
    uint32_t line = base_line;
    for (IrOp op : ops) {
        ir::IrInstr ins;
        ins.op = op;
        ins.type = IrType::I32;
        ins.source_line = line++;
        blk.instrs.push_back(ins);
    }
    fn.blocks.push_back(std::move(blk));
    return fn;
}

// =========================================================================
//  Tests de clasificacion de ops
// =========================================================================

static void test_classify_op() {
    std::printf("test_classify_op\n");

    // Pure native: aritmetica, control, memoria local, llamadas-linker.
    CHECK(aot_classify_op(IrOp::ADD) == AotOpClass::PURE_NATIVE);
    CHECK(aot_classify_op(IrOp::FMUL) == AotOpClass::PURE_NATIVE);
    CHECK(aot_classify_op(IrOp::BR_COND) == AotOpClass::PURE_NATIVE);
    CHECK(aot_classify_op(IrOp::ALLOCA) == AotOpClass::PURE_NATIVE);
    CHECK(aot_classify_op(IrOp::LOAD) == AotOpClass::PURE_NATIVE);
    CHECK(aot_classify_op(IrOp::STORE) == AotOpClass::PURE_NATIVE);
    CHECK(aot_classify_op(IrOp::GEP) == AotOpClass::PURE_NATIVE);
    CHECK(aot_classify_op(IrOp::MEMCPY) ==
          AotOpClass::PURE_NATIVE); // rep movsb inline
    CHECK(aot_classify_op(IrOp::CALL) == AotOpClass::PURE_NATIVE);
    CHECK(aot_classify_op(IrOp::CALLN) ==
          AotOpClass::PURE_NATIVE); // extern resuelto por linker
    CHECK(aot_classify_op(IrOp::INLINE_ASM) == AotOpClass::PURE_NATIVE);
    CHECK(aot_classify_op(IrOp::STR_LIT_ADDR) == AotOpClass::PURE_NATIVE);
    CHECK(aot_classify_op(IrOp::ATOMIC_CAS_I64) == AotOpClass::PURE_NATIVE);

    // Libc-mapped: dependencias sintetizadas por el compilador.
    CHECK(aot_classify_op(IrOp::RAW_ALLOC) == AotOpClass::LIBC_MAPPED);
    CHECK(aot_classify_op(IrOp::RAW_FREE) == AotOpClass::LIBC_MAPPED);
    CHECK(aot_classify_op(IrOp::PANIC) == AotOpClass::LIBC_MAPPED);
    CHECK(aot_classify_op(IrOp::SMARTPTR_FREE) == AotOpClass::LIBC_MAPPED);
    CHECK(std::string(aot_libc_symbol(IrOp::RAW_ALLOC)) == "malloc");
    CHECK(std::string(aot_libc_symbol(IrOp::RAW_FREE)) == "free");
    CHECK(std::string(aot_libc_symbol(IrOp::ADD)).empty());

    // Runtime-dependent.
    CHECK(aot_classify_op(IrOp::NEWOBJ) == AotOpClass::RUNTIME_DEPENDENT);
    CHECK(aot_classify_op(IrOp::STRMAKE) == AotOpClass::RUNTIME_DEPENDENT);
    CHECK(aot_classify_op(IrOp::FUTURE) == AotOpClass::RUNTIME_DEPENDENT);
    CHECK(aot_classify_op(IrOp::MONENTER) == AotOpClass::RUNTIME_DEPENDENT);
    CHECK(aot_classify_op(IrOp::THROW) == AotOpClass::RUNTIME_DEPENDENT);
    CHECK(aot_classify_op(IrOp::FINDCLASS) == AotOpClass::RUNTIME_DEPENDENT);
    CHECK(aot_classify_op(IrOp::RAW_ASM) == AotOpClass::RUNTIME_DEPENDENT);
}

// =========================================================================
//  Tests de admisibilidad por target
// =========================================================================

static void test_allowed_in_target() {
    std::printf("test_allowed_in_target\n");

    AotTarget bare;
    bare.tier = Tier::BARE;
    bare.freestanding = false;
    AotTarget freestand;
    freestand.tier = Tier::BARE;
    freestand.freestanding = true;
    AotTarget embed;
    embed.tier = Tier::EMBED;
    AotTarget full;
    full.tier = Tier::FULL;

    // Pure native: admisible en todos, incluido freestanding.
    CHECK(aot_op_allowed(IrOp::ADD, bare));
    CHECK(aot_op_allowed(IrOp::ADD, freestand));
    CHECK(aot_op_allowed(IrOp::ALLOCA,
                         freestand)); // stack = sub rsp, sin allocator
    CHECK(aot_op_allowed(IrOp::MEMCPY, freestand)); // rep movsb inline
    CHECK(aot_op_allowed(IrOp::CALLN,
                         freestand)); // extern a simbolo del kernel/usuario

    // Libc-mapped: OK en bare, RECHAZADA en freestanding (sin libc).
    CHECK(aot_op_allowed(IrOp::RAW_ALLOC, bare));
    CHECK(!aot_op_allowed(IrOp::RAW_ALLOC, freestand));
    CHECK(aot_op_allowed(IrOp::PANIC, bare));
    CHECK(!aot_op_allowed(IrOp::PANIC, freestand));

    // Runtime-dependent OOP: rechazada en bare, admitida en embed y full.
    CHECK(!aot_op_allowed(IrOp::NEWOBJ, bare));
    CHECK(aot_op_allowed(IrOp::NEWOBJ, embed));
    CHECK(aot_op_allowed(IrOp::NEWOBJ, full));
    CHECK(aot_op_allowed(IrOp::CALLVIRT, embed));
    CHECK(aot_op_allowed(IrOp::STRMAKE, embed)); // strings en embed

    // Async/distrib/scheduler: rechazada en embed, admitida en full.
    CHECK(!aot_op_allowed(IrOp::FUTURE, embed));
    CHECK(aot_op_allowed(IrOp::FUTURE, full));
    CHECK(!aot_op_allowed(IrOp::MSGSEND, embed));
    CHECK(aot_op_allowed(IrOp::MSGSEND, full));
    CHECK(!aot_op_allowed(IrOp::SPAWN, embed));
    CHECK(!aot_op_allowed(IrOp::FINDCLASS,
                          embed)); // classregistry dinamico no en embed

    // RAW_ASM: rechazada en TODOS los tiers (no es nativo).
    CHECK(!aot_op_allowed(IrOp::RAW_ASM, bare));
    CHECK(!aot_op_allowed(IrOp::RAW_ASM, embed));
    CHECK(!aot_op_allowed(IrOp::RAW_ASM, full));
}

// =========================================================================
//  Tests del analisis de modulo
// =========================================================================

static void test_analyze_module() {
    std::printf("test_analyze_module\n");

    AotTarget bare;
    bare.tier = Tier::BARE;
    AotTarget freestand;
    freestand.tier = Tier::BARE;
    freestand.freestanding = true;
    AotTarget embed;
    embed.tier = Tier::EMBED;
    AotTarget full;
    full.tier = Tier::FULL;

    // Modulo pure-native: compatible en bare Y freestanding.
    {
        ir::IrModule mod;
        mod.functions.push_back(
            make_fn("fib", {IrOp::CONST, IrOp::ADD, IrOp::SUB, IrOp::CMP_LT,
                            IrOp::BR_COND, IrOp::RET}));
        AotCompatReport r1 = aot_analyze_module(mod, bare);
        CHECK(r1.compatible);
        CHECK(r1.issues.empty());
        CHECK(r1.ok_functions.size() == 1);
        AotCompatReport r2 = aot_analyze_module(mod, freestand);
        CHECK(r2.compatible);
        CHECK(r2.render().empty());
    }

    // Modulo con malloc: compatible en bare, INCOMPATIBLE en freestanding.
    {
        ir::IrModule mod;
        mod.functions.push_back(
            make_fn("alloc_buf", {IrOp::CONST, IrOp::RAW_ALLOC, IrOp::RET}));
        AotCompatReport rb = aot_analyze_module(mod, bare);
        CHECK(rb.compatible);
        AotCompatReport rf = aot_analyze_module(mod, freestand);
        CHECK(!rf.compatible);
        CHECK(rf.issues.size() == 1);
        CHECK(rf.issues[0].op == IrOp::RAW_ALLOC);
        CHECK(rf.issues[0].source_line == 11); // segunda op (base 10 + 1)
        CHECK(rf.issues[0].fn_name == "alloc_buf");
        CHECK(!rf.render().empty());
    }

    // Modulo OOP: incompatible en bare, compatible en embed/full.
    {
        ir::IrModule mod;
        mod.functions.push_back(
            make_fn("make_obj",
                    {IrOp::NEWOBJ, IrOp::SETFIELD, IrOp::CALLVIRT, IrOp::RET}));
        AotCompatReport rb = aot_analyze_module(mod, bare);
        CHECK(!rb.compatible);
        CHECK(rb.issues.size() == 3); // NEWOBJ, SETFIELD, CALLVIRT
        AotCompatReport re = aot_analyze_module(mod, embed);
        CHECK(re.compatible);
        AotCompatReport rfu = aot_analyze_module(mod, full);
        CHECK(rfu.compatible);
    }

    // Modulo async: incompatible en embed, compatible en full.
    {
        ir::IrModule mod;
        mod.functions.push_back(
            make_fn("fetch", {IrOp::FUTURE, IrOp::AWAIT, IrOp::RET}));
        AotCompatReport re = aot_analyze_module(mod, embed);
        CHECK(!re.compatible);
        AotCompatReport rf = aot_analyze_module(mod, full);
        CHECK(rf.compatible);
    }

    // Stub nativo (is_native) siempre compatible (se resuelve en el linker).
    {
        ir::IrModule mod;
        ir::IrFunction stub;
        stub.name = "extern_fn";
        stub.is_native = true;
        mod.functions.push_back(std::move(stub));
        AotCompatReport r = aot_analyze_module(mod, freestand);
        CHECK(r.compatible);
        CHECK(r.ok_functions.size() == 1);
    }

    // Modulo mixto: una fn OK + una fn ofensora -> compatible=false pero
    // ok_functions lista la buena (util para compilar subset).
    {
        ir::IrModule mod;
        mod.functions.push_back(make_fn("pure_add", {IrOp::ADD, IrOp::RET}));
        mod.functions.push_back(make_fn("uses_gc", {IrOp::NEWOBJ, IrOp::RET}));
        AotCompatReport r = aot_analyze_module(mod, bare);
        CHECK(!r.compatible);
        CHECK(r.ok_functions.size() == 1);
        CHECK(r.ok_functions[0] == "pure_add");
        CHECK(r.issues.size() == 1);
        CHECK(r.issues[0].fn_name == "uses_gc");
    }
}

int main() {
    std::printf("=== test_aot_analyze ( AOT.1) ===\n");
    test_classify_op();
    test_allowed_in_target();
    test_analyze_module();
    std::printf("\n%d checks, %d fallos\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
