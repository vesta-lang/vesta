/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_vreg_native.cpp
 * @brief Phase AOT.3 Paso 2 -- valida @c vreg_compile_native (ABI HOST_LEAF).
 *
 * Construye funciones IR triviales, las compila con @c vreg_compile_native
 * (que produce BYTES nativos en ABI HOST_LEAF: args en arg_regs, retorno en
 * RAX, sin ProcessVM* ni runtime entries), copia esos bytes a un CodeCache
 * ejecutable y los INVOCA como funcion C nativa, comprobando el resultado.
 *
 * Es el oraculo del codegen AOT: el binario corre y devuelve lo correcto.
 */

#include "ir/ssa_ir.h"
#include "jit/code_cache.h"
#include "jit/vreg_pipeline.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

    int pass_count = 0;
    int fail_count = 0;

    #define CHECK(cond, msg) do {                                            \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL: %s (linea %d)\n", msg, __LINE__);    \
            ++fail_count;                                                    \
        } else {                                                             \
            ++pass_count;                                                    \
        }                                                                    \
    } while (0)

    /* Helpers para construir IR a mano (mismos que test_jit_compiler.cpp). */
    ir::IrValueId mk_value(ir::IrFunction &fn, ir::IrType t) {
        ir::IrValue v;
        v.type = t;
        fn.values.push_back(v);
        return static_cast<ir::IrValueId>(fn.values.size() - 1);
    }

    ir::IrInstr mk_instr(ir::IrOp op, ir::IrType t, ir::IrValueId dst) {
        ir::IrInstr i;
        i.op = op;
        i.type = t;
        i.dst = dst;
        return i;
    }

    /** @brief Copia @p bytes a un CodeCache ejecutable y devuelve el puntero. */
    uint8_t *make_runnable(jit::CodeCache &cc, const std::vector<uint8_t> &bytes) {
        if (bytes.empty()) return nullptr;
        uint8_t *code = cc.alloc(bytes.size(), 16);
        if (!code) return nullptr;
        std::memcpy(code, bytes.data(), bytes.size());
        cc.commit(code, bytes.size());
        return code;
    }

    /* ===================================================================== */
    /* Test 1: i64 ret42() { return 42; }  (el hito minimo de AOT)            */
    /* ===================================================================== */
    void test_ret42() {
        std::printf("[aot-native] return 42\n");
        ir::IrFunction fn;
        fn.name = "ret42";
        fn.ret_type = ir::IrType::I64;

        const ir::IrValueId v = mk_value(fn, ir::IrType::I64);
        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr c = mk_instr(ir::IrOp::CONST, ir::IrType::I64, v);
        c.imm = 42; entry.instrs.push_back(c);
        ir::IrInstr r = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        r.operands.push_back(v); entry.instrs.push_back(r);
        fn.blocks.push_back(entry);

        std::vector<uint8_t> bytes = jit::vreg_compile_native(fn);
        CHECK(!bytes.empty(), "vreg_compile_native produce bytes");

        jit::CodeCache cc;
        uint8_t *code = make_runnable(cc, bytes);
        CHECK(code != nullptr, "alloc + commit OK");
        if (code) {
            using F = int64_t(*)();
            const int64_t got = reinterpret_cast<F>(code)();
            CHECK(got == 42, "ret42() == 42");
            if (got != 42) std::printf("    obtuvo %lld\n", (long long)got);
        }
    }

    /* ===================================================================== */
    /* Test 2: i64 sum() { return 40 + 2; }  (ALU + RET en RAX)               */
    /* ===================================================================== */
    void test_add() {
        std::printf("[aot-native] 40 + 2 = 42\n");
        ir::IrFunction fn;
        fn.name = "sum40_2";
        fn.ret_type = ir::IrType::I64;

        const ir::IrValueId a = mk_value(fn, ir::IrType::I64);
        const ir::IrValueId b = mk_value(fn, ir::IrType::I64);
        const ir::IrValueId t = mk_value(fn, ir::IrType::I64);
        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr ca = mk_instr(ir::IrOp::CONST, ir::IrType::I64, a);
        ca.imm = 40; entry.instrs.push_back(ca);
        ir::IrInstr cb = mk_instr(ir::IrOp::CONST, ir::IrType::I64, b);
        cb.imm = 2; entry.instrs.push_back(cb);
        ir::IrInstr add = mk_instr(ir::IrOp::ADD, ir::IrType::I64, t);
        add.operands.push_back(a); add.operands.push_back(b);
        entry.instrs.push_back(add);
        ir::IrInstr r = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        r.operands.push_back(t); entry.instrs.push_back(r);
        fn.blocks.push_back(entry);

        std::vector<uint8_t> bytes = jit::vreg_compile_native(fn);
        CHECK(!bytes.empty(), "vreg_compile_native produce bytes (add)");

        jit::CodeCache cc;
        uint8_t *code = make_runnable(cc, bytes);
        CHECK(code != nullptr, "alloc + commit OK (add)");
        if (code) {
            using F = int64_t(*)();
            const int64_t got = reinterpret_cast<F>(code)();
            CHECK(got == 42, "sum40_2() == 42");
            if (got != 42) std::printf("    obtuvo %lld\n", (long long)got);
        }
    }

} // namespace

int main() {
    std::printf("=== test_vreg_native (Phase AOT.3 Paso 2) ===\n");
    test_ret42();
    test_add();
    std::printf("\n%d checks OK, %d fallos\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
