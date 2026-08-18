/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_inline_asm.cpp
 * @brief  AS inc.5 (AS.10): test del codegen de inline-asm en el path
 *        vreg (VM_ABI).  Construye funciones IR con @c IrOp::INLINE_ASM +
 *        @c asm_reg_bindings (igual que el lowering de ), las compila por el
 *        pipeline vreg (selector -> intervalos -> linear-scan -> rewrite ->
 *        encoder) y las EJECUTA con un proxy del ProcessVM, verificando el
 *        resultado.
 *
 * Usa un @c AsmBackend STUB que devuelve bytes x86-64 hardcodeados para las
 * pocas instrucciones del test -- aisla el codegen NUEVO (pin de register() via
 * precoloreo + emision de INLINE_ASM_RAW + supervivencia del blob) del
 * ensamblado real (Keystone, ya validado en inc.4b).  El proxy reproduce el
 * layout que el codigo VM_ABI espera (safepoint_flag@0, registers@96,
 * RBX=proc).
 */

#include "ir/ssa_ir.h"
#include "jit/code_cache.h"
#include "jit/interval.h"
#include "codegen/regalloc.h"
#include "codegen/rbank/allocate.h"
#include "jit/machine_ir.h"
#include "jit/regalloc_rewrite.h"
#include "codegen/timeline_builder.h"
#include "jit/target_reginfo.h"
#include "jit/vreg_select.h"
#include "jit/x86_encoder.h"
#include "vesta_rt/abi.h"
#include "vx/asm/asm_backend.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace jit;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fails;                                                         \
            std::printf("  FAIL: %s  (linea %d)\n", (msg), __LINE__);          \
        }                                                                      \
    } while (0)

/** @brief Proxy del ProcessVM (mismo layout que el codigo JIT VM_ABI espera).
 */
struct Proxy {
    uint8_t safepoint_flag;                        // offset 0
    uint8_t _pad[VESTA_PROC_REGISTERS_OFFSET - 1]; // hasta offset 96
    uint64_t regs[VESTA_PROC_REGISTER_COUNT];      // offset 96
};
static_assert(offsetof(Proxy, regs) == VESTA_PROC_REGISTERS_OFFSET,
              "regs debe quedar en el offset que usa el codigo JIT");

/** @brief Backend de ensamblado STUB: tabla nasm-string -> bytes. */
struct StubAsm : vx::AsmBackend {
    vx::AsmAssembleResult assemble(const std::string &nasm,
                                   vx::AsmArch) override {
        vx::AsmAssembleResult r;
        r.ok = true;
        if (nasm == "popcnt rax, rdi") {
            r.bytes = {0xF3, 0x48, 0x0F, 0xB8, 0xC7}; // popcnt rax, rdi
        } else if (nasm == "add rax, rax") {
            r.bytes = {0x48, 0x01, 0xC0}; // add rax, rax
        } else if (nasm == "lea rax, [rdi + rsi]") {
            r.bytes = {0x48, 0x8D, 0x04, 0x37}; // lea rax, [rdi+rsi]
        } else {
            r.ok = false;
            r.error = "stub: nasm no reconocido";
        }
        return r;
    }
};

/* ---- Helpers IR para inline-asm ---------------------------------------- */
static ir::IrInstr alloca_bind(ir::IrValueId d) {
    ir::IrInstr i;
    i.op = ir::IrOp::ALLOCA;
    i.type = ir::IrType::I64;
    i.dst = d;
    i.imm = 8;
    i.host_alloca = true;
    return i;
}
static ir::IrInstr store_to(ir::IrValueId val, ir::IrValueId ptr) {
    ir::IrInstr i;
    i.op = ir::IrOp::STORE;
    i.type = ir::IrType::I64;
    i.operands = {val, ptr};
    return i; // [0]=val [1]=ptr
}
static ir::IrInstr load_from(ir::IrValueId d, ir::IrValueId ptr) {
    ir::IrInstr i;
    i.op = ir::IrOp::LOAD;
    i.type = ir::IrType::I64;
    i.dst = d;
    i.operands = {ptr};
    return i;
}
static ir::IrInstr inline_asm(const std::string &body,
                              std::vector<ir::IrValueId> ops) {
    ir::IrInstr i;
    i.op = ir::IrOp::INLINE_ASM;
    i.type = ir::IrType::VOID;
    i.func_name = body;
    i.operands = std::move(ops);
    i.imm = 0;
    return i;
}
static ir::IrInstr ret1(ir::IrValueId v) {
    ir::IrInstr i;
    i.op = ir::IrOp::RET;
    i.type = ir::IrType::I64;
    i.operands = {v};
    return i;
}

/** @brief Compila @p fn (VM_ABI) y la ejecuta con @p px (RBX=&px). */
static bool run_vm(const ir::IrFunction &fn, Proxy &px) {
    MFunction mf;
    VregEntries ent;
    if (!vreg_select(fn, mf, AbiKind::VM, {}, ent, {}, {})) {
        std::printf("  (vreg_select rechazo la funcion)\n");
        return false;
    }
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    codegen::RegAlloc ra = codegen::rbank::rbank_allocate(
        build_intervals(mf, tri), mf.vreg_count, tri, false);
    MFunction pf =
        rewrite_to_physical(mf,
                            codegen::build_allocation_result(
                                ra, nullptr, codegen::AssignmentPlan{}),
                            tri, AbiKind::VM);
    X86Encoder enc;
    std::vector<uint8_t> bytes;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) {
        std::printf("  (encoder no produjo bytes)\n");
        return false;
    }
    CodeCache cc;
    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return false;
    std::memcpy(code, bytes.data(), bytes.size());
    cc.commit(code, bytes.size());
    reinterpret_cast<void (*)(void *)>(code)(&px);
    asm volatile("" : : "r"(&cc) : "memory"); // mantener cc viva
    return true;
}

/* ---- Test 1: popcount via inline-asm (1 input rdi, 1 output rax) -------- */
static void test_popcount() {
    std::printf("[asm] popcount(x): regs[1]=0xFF -> regs[0]=8\n");
    ir::IrFunction fn;
    fn.name = "popcount_asm";
    fn.ret_type = ir::IrType::I64;
    auto x = fn.new_value(ir::IrType::I64);
    auto a_in = fn.new_value(ir::IrType::I64);  // alloca -> binding rdi
    auto a_res = fn.new_value(ir::IrType::I64); // alloca -> binding rax
    auto r = fn.new_value(ir::IrType::I64);
    fn.params = {x};
    fn.asm_reg_bindings = {
        {a_in, "rdi", ir::IrType::I64, false, "in_v"},
        {a_res, "rax", ir::IrType::I64, false, "result"},
    };
    auto bb = fn.new_block("e");
    fn.append(bb, alloca_bind(a_in));
    fn.append(bb, store_to(x, a_in)); // rdi = x
    fn.append(bb, alloca_bind(a_res));
    fn.append(bb, inline_asm("popcnt rax, rdi", {a_in, a_res}));
    fn.append(bb, load_from(r, a_res)); // r = rax
    fn.append(bb, ret1(r));

    Proxy px;
    std::memset(&px, 0, sizeof(px));
    px.regs[1] = 0xFF; // 8 bits set
    if (run_vm(fn, px)) {
        std::printf("  regs[0] = %llu\n", (unsigned long long)px.regs[0]);
        CHECK(px.regs[0] == 8, "popcount(0xFF) debe ser 8");
    } else {
        CHECK(false, "popcount no compilo/ejecuto");
    }
}

/* ---- Test 2: double via inout binding (rax in+out) --------------------- */
static void test_double_inout() {
    std::printf("[asm] double(x): regs[1]=21 -> regs[0]=42 (rax inout)\n");
    ir::IrFunction fn;
    fn.name = "double_asm";
    fn.ret_type = ir::IrType::I64;
    auto x = fn.new_value(ir::IrType::I64);
    auto a = fn.new_value(ir::IrType::I64); // alloca -> binding rax (inout)
    auto r = fn.new_value(ir::IrType::I64);
    fn.params = {x};
    fn.asm_reg_bindings = {
        {a, "rax", ir::IrType::I64, false, "acc"},
    };
    auto bb = fn.new_block("e");
    fn.append(bb, alloca_bind(a));
    fn.append(bb, store_to(x, a)); // rax = x
    fn.append(bb, inline_asm("add rax, rax", {a}));
    fn.append(bb, load_from(r, a)); // r = rax
    fn.append(bb, ret1(r));

    Proxy px;
    std::memset(&px, 0, sizeof(px));
    px.regs[1] = 21;
    if (run_vm(fn, px)) {
        std::printf("  regs[0] = %llu\n", (unsigned long long)px.regs[0]);
        CHECK(px.regs[0] == 42, "double(21) debe ser 42");
    } else {
        CHECK(false, "double no compilo/ejecuto");
    }
}

/* ---- Test 3: add(a,b) via inline-asm (2 inputs rdi/rsi, 1 output rax) --- */
static void test_add_two() {
    std::printf("[asm] add(a,b): regs[1]=40, regs[2]=2 -> regs[0]=42\n");
    ir::IrFunction fn;
    fn.name = "add_asm";
    fn.ret_type = ir::IrType::I64;
    auto a = fn.new_value(ir::IrType::I64);
    auto b = fn.new_value(ir::IrType::I64);
    auto a_lhs = fn.new_value(ir::IrType::I64); // binding rdi
    auto a_rhs = fn.new_value(ir::IrType::I64); // binding rsi
    auto a_res = fn.new_value(ir::IrType::I64); // binding rax
    auto r = fn.new_value(ir::IrType::I64);
    fn.params = {a, b};
    fn.asm_reg_bindings = {
        {a_lhs, "rdi", ir::IrType::I64, false, "lhs"},
        {a_rhs, "rsi", ir::IrType::I64, false, "rhs"},
        {a_res, "rax", ir::IrType::I64, false, "res"},
    };
    auto bb = fn.new_block("e");
    fn.append(bb, alloca_bind(a_lhs));
    fn.append(bb, store_to(a, a_lhs)); // rdi = a
    fn.append(bb, alloca_bind(a_rhs));
    fn.append(bb, store_to(b, a_rhs)); // rsi = b
    fn.append(bb, alloca_bind(a_res));
    fn.append(bb, inline_asm("lea rax, [rdi + rsi]", {a_lhs, a_rhs, a_res}));
    fn.append(bb, load_from(r, a_res));
    fn.append(bb, ret1(r));

    Proxy px;
    std::memset(&px, 0, sizeof(px));
    px.regs[1] = 40;
    px.regs[2] = 2;
    if (run_vm(fn, px)) {
        std::printf("  regs[0] = %llu\n", (unsigned long long)px.regs[0]);
        CHECK(px.regs[0] == 42, "add(40,2) debe ser 42");
    } else {
        CHECK(false, "add no compilo/ejecuto");
    }
}

int main() {
    std::printf("=== test_inline_asm ( AS inc.5 / AS.10) ===\n");
    static StubAsm stub;
    vx::g_asm_backend = &stub; // registrar el backend para vreg_select

    test_popcount();
    test_double_inout();
    test_add_two();

    std::printf("\n%d checks, %d fallos\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
