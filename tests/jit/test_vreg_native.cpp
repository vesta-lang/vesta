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

    /* ===================================================================== */
    /* Test 3: i64 add2(a, b) { return a + b; }  (params en arg_regs)         */
    /* ===================================================================== */
    void test_param_add2() {
        std::printf("[aot-native] add2(a, b) = a + b\n");
        ir::IrFunction fn;
        fn.name = "add2";
        fn.ret_type = ir::IrType::I64;
        const ir::IrValueId a = mk_value(fn, ir::IrType::I64);
        const ir::IrValueId b = mk_value(fn, ir::IrType::I64);
        fn.params = {a, b};
        const ir::IrValueId t = mk_value(fn, ir::IrType::I64);

        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr add = mk_instr(ir::IrOp::ADD, ir::IrType::I64, t);
        add.operands.push_back(a); add.operands.push_back(b);
        entry.instrs.push_back(add);
        ir::IrInstr r = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        r.operands.push_back(t); entry.instrs.push_back(r);
        fn.blocks.push_back(entry);

        std::vector<uint8_t> bytes = jit::vreg_compile_native(fn);
        CHECK(!bytes.empty(), "vreg_compile_native produce bytes (add2)");
        jit::CodeCache cc;
        uint8_t *code = make_runnable(cc, bytes);
        CHECK(code != nullptr, "alloc + commit OK (add2)");
        if (code) {
            using F = int64_t(*)(int64_t, int64_t);
            const int64_t got = reinterpret_cast<F>(code)(40, 2);
            CHECK(got == 42, "add2(40, 2) == 42");
            if (got != 42) std::printf("    obtuvo %lld\n", (long long)got);
            CHECK(reinterpret_cast<F>(code)(100, -58) == 42, "add2(100,-58) == 42");
        }
    }

    /* ===================================================================== */
    /* Test 4: i64 combine(a, b, c, d) { return a - b + c * d; }              *
     * 4 args (cubre todos los arg_regs de Win64) + orden no trivial: estresa *
     * el parallel-move de la carga de params.                                 */
    /* ===================================================================== */
    void test_param_combine4() {
        std::printf("[aot-native] combine(a,b,c,d) = a - b + c*d\n");
        ir::IrFunction fn;
        fn.name = "combine4";
        fn.ret_type = ir::IrType::I64;
        const ir::IrValueId a = mk_value(fn, ir::IrType::I64);
        const ir::IrValueId b = mk_value(fn, ir::IrType::I64);
        const ir::IrValueId c = mk_value(fn, ir::IrType::I64);
        const ir::IrValueId d = mk_value(fn, ir::IrType::I64);
        fn.params = {a, b, c, d};
        const ir::IrValueId ab  = mk_value(fn, ir::IrType::I64);  // a - b
        const ir::IrValueId cd  = mk_value(fn, ir::IrType::I64);  // c * d
        const ir::IrValueId res = mk_value(fn, ir::IrType::I64);  // ab + cd

        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr sub = mk_instr(ir::IrOp::SUB, ir::IrType::I64, ab);
        sub.operands.push_back(a); sub.operands.push_back(b);
        entry.instrs.push_back(sub);
        ir::IrInstr mul = mk_instr(ir::IrOp::MUL, ir::IrType::I64, cd);
        mul.operands.push_back(c); mul.operands.push_back(d);
        entry.instrs.push_back(mul);
        ir::IrInstr addf = mk_instr(ir::IrOp::ADD, ir::IrType::I64, res);
        addf.operands.push_back(ab); addf.operands.push_back(cd);
        entry.instrs.push_back(addf);
        ir::IrInstr r = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        r.operands.push_back(res); entry.instrs.push_back(r);
        fn.blocks.push_back(entry);

        std::vector<uint8_t> bytes = jit::vreg_compile_native(fn);
        CHECK(!bytes.empty(), "vreg_compile_native produce bytes (combine4)");
        jit::CodeCache cc;
        uint8_t *code = make_runnable(cc, bytes);
        CHECK(code != nullptr, "alloc + commit OK (combine4)");
        if (code) {
            using F = int64_t(*)(int64_t, int64_t, int64_t, int64_t);
            // 10 - 8 + 5*8 = 2 + 40 = 42
            const int64_t got = reinterpret_cast<F>(code)(10, 8, 5, 8);
            CHECK(got == 42, "combine4(10,8,5,8) == 42");
            if (got != 42) std::printf("    obtuvo %lld\n", (long long)got);
            // 100 - 100 + 6*7 = 42
            CHECK(reinterpret_cast<F>(code)(100, 100, 6, 7) == 42,
                  "combine4(100,100,6,7) == 42");
        }
    }

} // namespace

int main() {
    std::printf("=== test_vreg_native (Phase AOT.3 Paso 2) ===\n");
    test_ret42();
    test_add();
    test_param_add2();
    test_param_combine4();
    std::printf("\n%d checks OK, %d fallos\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
