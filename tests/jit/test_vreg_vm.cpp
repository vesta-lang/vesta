/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_vreg_vm.cpp
 * @brief Test del path vreg en modo VM_ABI (Phase D.7, commit 5a).
 *
 * Compila funciones IR con la convencion VM_ABI (args en
 * @c proc->registers.regs[1..N], return en @c regs[0], @c ProcessVM* en RBX)
 * y las ejecuta usando un PROXY que reproduce el layout (safepoint_flag en
 * offset 0, registers en offset 96), sin arrastrar el runtime completo.
 * Valida una funcion con args y un loop con args.
 */

#include "ir/ssa_ir.h"
#include "jit/code_cache.h"
#include "jit/interval.h"
#include "jit/linear_scan.h"
#include "jit/machine_ir.h"
#include "jit/regalloc_rewrite.h"
#include "jit/target_reginfo.h"
#include "jit/vreg_select.h"
#include "jit/x86_encoder.h"
#include "vesta_rt/abi.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace jit;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                    \
    do { ++g_checks; if (!(cond)) { ++g_fails;                              \
        std::printf("  FAIL: %s  (linea %d)\n", (msg), __LINE__); } } while (0)

/** @brief Proxy que reproduce el layout que el codigo JIT espera (RBX). */
struct Proxy {
    uint8_t  safepoint_flag;                               // offset 0
    uint8_t  _pad[VESTA_PROC_REGISTERS_OFFSET - 1];        // hasta offset 96
    uint64_t regs[VESTA_PROC_REGISTER_COUNT];              // offset 96
};
static_assert(offsetof(Proxy, regs) == VESTA_PROC_REGISTERS_OFFSET,
              "regs debe quedar en el offset que usa el codigo JIT");

/* Helpers IR. */
static ir::IrInstr konst(ir::IrValueId d, int64_t k) {
    ir::IrInstr i; i.op = ir::IrOp::CONST; i.type = ir::IrType::I64;
    i.dst = d; i.imm = (uint64_t)k; return i;
}
static ir::IrInstr bin(ir::IrOp op, ir::IrValueId d, ir::IrValueId a, ir::IrValueId b) {
    ir::IrInstr i; i.op = op; i.type = ir::IrType::I64;
    i.dst = d; i.operands = { a, b }; return i;
}
static ir::IrInstr ret1(ir::IrValueId v) {
    ir::IrInstr i; i.op = ir::IrOp::RET; i.type = ir::IrType::I64;
    i.operands = { v }; return i;
}
static ir::IrInstr cmp(ir::IrOp op, ir::IrValueId d, ir::IrValueId a, ir::IrValueId b) {
    ir::IrInstr i; i.op = op; i.type = ir::IrType::BOOL;
    i.dst = d; i.operands = { a, b }; return i;
}
static ir::IrInstr br(ir::IrBlockId t) {
    ir::IrInstr i; i.op = ir::IrOp::BR; i.target_block = t; return i;
}
static ir::IrInstr brc(ir::IrValueId c, ir::IrBlockId t, ir::IrBlockId f) {
    ir::IrInstr i; i.op = ir::IrOp::BR_COND; i.operands = { c };
    i.target_block = t; i.false_block = f; return i;
}
static ir::IrInstr phi2(ir::IrValueId d, ir::IrValueId v0, ir::IrBlockId b0,
                        ir::IrValueId v1, ir::IrBlockId b1) {
    ir::IrInstr i; i.op = ir::IrOp::PHI; i.type = ir::IrType::I64; i.dst = d;
    i.phi_args = { { v0, b0 }, { v1, b1 } }; return i;
}
static ir::IrInstr conv(ir::IrOp op, ir::IrValueId d, ir::IrValueId s, ir::IrType dt) {
    ir::IrInstr i; i.op = op; i.type = dt; i.dst = d; i.operands = { s }; return i;
}

/** @brief Compila @p fn en modo VM y la ejecuta con @p px (RBX = &px). */
static bool jit_vm(const ir::IrFunction &fn, Proxy &px,
                   const CallResolver &resolve = {}, uint64_t callvirt_addr = 0,
                   const CallResolver &resolve_native = {}) {
    MFunction mf;
    VregEntries ent; ent.callvirt = callvirt_addr;
    if (!vreg_select(fn, mf, AbiKind::VM, resolve, ent, resolve_native)) return false;
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM);
    X86Encoder enc;
    std::vector<uint8_t> bytes;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) return false;
    CodeCache cc;
    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return false;
    std::memcpy(code, bytes.data(), bytes.size());
    cc.commit(code, bytes.size());
    reinterpret_cast<void (*)(void *)>(code)(&px);
    asm volatile("" : : "r"(&cc) : "memory");  // mantener cc viva hasta aqui
    return true;
}

/* ---- Test 1: add(a,b) con args desde proc->registers ------------------ */
static void test_vm_add() {
    std::printf("[vm] add(a,b): regs[1]=40, regs[2]=2 -> regs[0]=42\n");
    ir::IrFunction fn;
    fn.name = "add"; fn.ret_type = ir::IrType::I64;
    auto a = fn.new_value(ir::IrType::I64);
    auto b = fn.new_value(ir::IrType::I64);
    auto c = fn.new_value(ir::IrType::I64);
    fn.params = { a, b };
    auto bb = fn.new_block("e");
    fn.append(bb, bin(ir::IrOp::ADD, c, a, b));
    fn.append(bb, ret1(c));

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 40; px.regs[2] = 2;
    CHECK(jit_vm(fn, px), "jit_vm ok (add)");
    CHECK(px.regs[0] == 42, "add(40,2)==42 en regs[0]");
    if (px.regs[0] != 42) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test 2: loop con arg -> sum(0..n-1), n en regs[1] ---------------- */
static void test_vm_loop() {
    std::printf("[vm] sum(n): regs[1]=10 -> regs[0]=45\n");
    ir::IrFunction fn;
    fn.name = "sum_n"; fn.ret_type = ir::IrType::I64;
    auto T = ir::IrType::I64;
    ir::IrValueId n = fn.new_value(T);            // param
    ir::IrValueId zero = fn.new_value(T), one = fn.new_value(T);
    ir::IrValueId i = fn.new_value(T), sum = fn.new_value(T);
    ir::IrValueId cond = fn.new_value(ir::IrType::BOOL);
    ir::IrValueId sum_next = fn.new_value(T), i_next = fn.new_value(T);
    fn.params = { n };

    ir::IrBlockId b0 = fn.new_block("entry");
    ir::IrBlockId b1 = fn.new_block("header");
    ir::IrBlockId b2 = fn.new_block("body");
    ir::IrBlockId b3 = fn.new_block("exit");

    fn.append(b0, konst(zero, 0)); fn.append(b0, konst(one, 1)); fn.append(b0, br(b1));
    fn.append(b1, phi2(i, zero, b0, i_next, b2));
    fn.append(b1, phi2(sum, zero, b0, sum_next, b2));
    fn.append(b1, cmp(ir::IrOp::CMP_LT, cond, i, n));
    fn.append(b1, brc(cond, b2, b3));
    fn.append(b2, bin(ir::IrOp::ADD, sum_next, sum, i));
    fn.append(b2, bin(ir::IrOp::ADD, i_next, i, one));
    fn.append(b2, br(b1));
    fn.append(b3, ret1(sum));

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 10;
    CHECK(jit_vm(fn, px), "jit_vm ok (loop)");
    CHECK(px.regs[0] == 45, "sum(0..9)==45 en regs[0]");
    if (px.regs[0] != 45) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test 3: loop con SEXT loop-invariante (repro widen) ------------- *
 * widen(y): acc=0; for(i=0;i<1000;i++) acc += (i64)y;  return acc.
 * Con y=7 -> 7000.  El (i64)y es invariante (SEXT en entry); el i<1000
 * usa SEXT del contador. */
static void test_vm_sext_loop() {
    std::printf("[vm] widen(7): regs[1]=7 -> regs[0]=7000\n");
    ir::IrFunction fn;
    fn.name = "widen"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64; auto I32 = ir::IrType::I32;
    ir::IrValueId thisp  = fn.new_value(ir::IrType::PTR);  // param 0 (this, no usado)
    ir::IrValueId y      = fn.new_value(I32);   // param 1
    ir::IrValueId acc0   = fn.new_value(I64), i0 = fn.new_value(I32);
    ir::IrValueId binv   = fn.new_value(I64);   // (i64)y invariante
    ir::IrValueId one    = fn.new_value(I32), n = fn.new_value(I64);
    ir::IrValueId i_phi  = fn.new_value(I32), acc_phi = fn.new_value(I64);
    ir::IrValueId i_ext  = fn.new_value(I64);
    ir::IrValueId cond   = fn.new_value(ir::IrType::BOOL);
    ir::IrValueId acc_n  = fn.new_value(I64), i_n = fn.new_value(I32);
    fn.params = { thisp, y };   // this=param0 (regs[1]), y=param1 (regs[2])

    ir::IrBlockId b0 = fn.new_block("entry");
    ir::IrBlockId b1 = fn.new_block("header");
    ir::IrBlockId b2 = fn.new_block("body");
    ir::IrBlockId b3 = fn.new_block("exit");

    auto konst_i32 = [](ir::IrValueId d, int64_t k) {
        ir::IrInstr i; i.op = ir::IrOp::CONST; i.type = ir::IrType::I32;
        i.dst = d; i.imm = static_cast<uint64_t>(k); return i;
    };
    fn.append(b0, konst(acc0, 0));
    fn.append(b0, konst_i32(i0, 0));
    fn.append(b0, conv(ir::IrOp::SEXT, binv, y, I64));   // binv = (i64)y
    fn.append(b0, konst_i32(one, 1));
    fn.append(b0, konst(n, 1000));
    fn.append(b0, br(b1));

    fn.append(b1, phi2(i_phi, i0, b0, i_n, b2));
    fn.append(b1, phi2(acc_phi, acc0, b0, acc_n, b2));
    fn.append(b1, conv(ir::IrOp::SEXT, i_ext, i_phi, I64));
    fn.append(b1, cmp(ir::IrOp::CMP_LT, cond, i_ext, n));
    fn.append(b1, brc(cond, b2, b3));

    fn.append(b2, bin(ir::IrOp::ADD, acc_n, acc_phi, binv));
    {
        ir::IrInstr a; a.op = ir::IrOp::ADD; a.type = I32;
        a.dst = i_n; a.operands = { i_phi, one }; fn.append(b2, a);
    }
    fn.append(b2, br(b1));

    fn.append(b3, ret1(acc_phi));

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 0x1234;  // this (no usado)
    px.regs[2] = 7;       // y
    CHECK(jit_vm(fn, px), "jit_vm ok (widen)");
    CHECK(px.regs[0] == 7000, "widen(7)==7000 (SEXT invariante)");
    if (px.regs[0] != 7000) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/** @brief Callee de prueba con convencion VM: lee args de proc->registers
 *  (regs[1], regs[2]) y escribe el resultado en regs[0].  a*3 + b. */
extern "C" void vm_helper(void *proc) {
    Proxy *p = static_cast<Proxy *>(proc);
    p->regs[0] = p->regs[1] * 3 + p->regs[2];
}

/* ---- Test 4: CALL a otra funcion (convencion VM) --------------------- *
 * caller(a,b) = helper(a,b) = a*3 + b.  Con a=10,b=5 -> 35. */
static void test_vm_call() {
    std::printf("[vm] caller(10,5) -> helper -> regs[0]=35\n");
    ir::IrFunction fn;
    fn.name = "caller"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId a = fn.new_value(I64), b = fn.new_value(I64);
    ir::IrValueId r = fn.new_value(I64);
    fn.params = { a, b };
    ir::IrBlockId bb = fn.new_block("e");
    {
        ir::IrInstr c; c.op = ir::IrOp::CALL; c.type = I64; c.dst = r;
        c.func_name = "helper"; c.operands = { a, b };
        fn.append(bb, c);
    }
    fn.append(bb, ret1(r));

    CallResolver resolver = [](const std::string &name) -> uint64_t {
        if (name == "helper")
            return reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&vm_helper));
        return 0;
    };

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 10; px.regs[2] = 5;
    CHECK(jit_vm(fn, px, resolver), "jit_vm ok (call)");
    CHECK(px.regs[0] == 35, "caller(10,5)==35 (CALL VM)");
    if (px.regs[0] != 35) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/** @brief Stub de vrt_callvirt: lee args de proc->registers (regs[2]=arg) +
 *  vtbl_idx, escribe el resultado en regs[0].  arg*10 + idx. */
extern "C" uint64_t vm_callvirt_stub(void *proc, void * /*obj*/, uint32_t idx) {
    Proxy *p = static_cast<Proxy *>(proc);
    p->regs[0] = p->regs[2] * 10 + idx;
    return p->regs[0];
}

/* ---- Test 5: CALLVIRT (dispatch via vrt_callvirt stub) --------------- *
 * caller(this,x) = callvirt(this, idx=5)(x) = x*10 + 5.  Con x=7 -> 75. */
static void test_vm_callvirt() {
    std::printf("[vm] caller(this,7) -> callvirt idx=5 -> regs[0]=75\n");
    ir::IrFunction fn;
    fn.name = "cv_caller"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId thisp = fn.new_value(ir::IrType::PTR);
    ir::IrValueId x = fn.new_value(I64), r = fn.new_value(I64);
    fn.values[thisp].is_gc_object = true;   // receptor GC (muere en el call)
    fn.params = { thisp, x };
    ir::IrBlockId bb = fn.new_block("e");
    {
        ir::IrInstr c; c.op = ir::IrOp::CALLVIRT; c.type = I64; c.dst = r;
        c.imm = 5; c.operands = { thisp, x };  // operands[0]=this, [1]=x; imm=vtbl_idx
        fn.append(bb, c);
    }
    fn.append(bb, ret1(r));

    const uint64_t cv = reinterpret_cast<uint64_t>(
        reinterpret_cast<void *>(&vm_callvirt_stub));

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 0xABCD; px.regs[2] = 7;
    CHECK(jit_vm(fn, px, {}, cv), "jit_vm ok (callvirt)");
    CHECK(px.regs[0] == 75, "caller(this,7)==75 (CALLVIRT)");
    if (px.regs[0] != 75) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/** @brief Callee de prueba: pone regs[0]=50 (simula trabajo + clobbea
 *  caller-saved como cualquier funcion C). */
extern "C" void g_stub(void *proc) {
    Proxy *p = static_cast<Proxy *>(proc);
    p->regs[0] = 50;
}

/* ---- Test 6 (commit 6): GC root vivo a traves de un call --------------- *
 * gcf(this_gc, x) = g() + this.  `this` (GC, host_ptr) se usa DESPUES del
 * call -> vivo a traves -> el allocator lo SPILLEA a slot y emite un stackmap
 * que lo describe.  Verifica: (1) compila, (2) this spilled, (3) stackmap con
 * 1 slot HOSTPTR, (4) ejecuta (el spill/reload preserva el valor): 50+7=57. */
static void test_vm_gc_stackmap() {
    std::printf("[vm] GC root vivo a traves de call -> spilled + stackmap\n");
    ir::IrFunction fn;
    fn.name = "gcf"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId thisp = fn.new_value(ir::IrType::PTR);
    ir::IrValueId x = fn.new_value(I64), r = fn.new_value(I64), sum = fn.new_value(I64);
    fn.values[thisp].is_gc_object = true;
    fn.values[thisp].is_host_ptr  = true;   // -> StackmapGcKind::HOSTPTR
    fn.params = { thisp, x };
    ir::IrBlockId bb = fn.new_block("e");
    {
        ir::IrInstr c; c.op = ir::IrOp::CALL; c.type = I64; c.dst = r;
        c.func_name = "g";  // sin args
        fn.append(bb, c);
    }
    fn.append(bb, bin(ir::IrOp::ADD, sum, r, thisp));  // this vivo a traves del call
    fn.append(bb, ret1(sum));

    CallResolver resolver = [](const std::string &n) -> uint64_t {
        if (n == "g") return reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&g_stub));
        return 0;
    };

    MFunction mf;
    CHECK(vreg_select(fn, mf, AbiKind::VM, resolver), "vreg_select ok (gcf)");
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    IntervalResult ivs = build_intervals(mf, tri);
    RegAlloc ra = linear_scan(ivs, tri);
    CHECK(ra.spilled(thisp), "this (GC root vivo a traves) spilled a slot");
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM, &ivs);
    CHECK(pf.stackmaps.size() == 1, "1 stackmap (1 call)");
    if (pf.stackmaps.size() == 1) {
        CHECK(pf.stackmaps[0].slots.size() == 1, "stackmap describe 1 GC root");
        if (pf.stackmaps[0].slots.size() == 1)
            CHECK(pf.stackmaps[0].slots[0].gc_kind == StackmapGcKind::HOSTPTR,
                  "gc_kind == HOSTPTR");
    }
    X86Encoder enc; std::vector<uint8_t> bytes;
    CHECK(enc.encode(pf, bytes) != 0 && !bytes.empty(), "encode ok");
    CodeCache cc; uint8_t *code = cc.alloc(bytes.size(), 16);
    std::memcpy(code, bytes.data(), bytes.size()); cc.commit(code, bytes.size());
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 7; px.regs[2] = 99;   // this=7, x=99 (no usado)
    reinterpret_cast<void (*)(void *)>(code)(&px);
    asm volatile("" : : "r"(&cc) : "memory");
    CHECK(px.regs[0] == 57, "gcf == g()(50) + this(7) == 57");
    if (px.regs[0] != 57) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test 7 (commit 7): LOAD/STORE sobre memoria host ----------------- *
 * ls(ptr) carga buf[0], le suma 100, lo guarda de vuelta y lo retorna.
 * buf[0]=5 -> buf[0]=105, ret 105. */
static void test_vm_load_store() {
    std::printf("[vm] load/store host: buf[0]=5 -> +100 -> 105\n");
    ir::IrFunction fn;
    fn.name = "ls"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId ptr = fn.new_value(ir::IrType::PTR);
    ir::IrValueId v = fn.new_value(I64), c = fn.new_value(I64), w = fn.new_value(I64);
    fn.values[ptr].is_host_ptr = true;
    fn.params = { ptr };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr i; i.op = ir::IrOp::LOAD; i.type = I64; i.dst = v;
      i.operands = { ptr }; fn.append(bb, i); }
    fn.append(bb, konst(c, 100));
    fn.append(bb, bin(ir::IrOp::ADD, w, v, c));
    { ir::IrInstr i; i.op = ir::IrOp::STORE; i.type = I64;
      i.operands = { w, ptr }; fn.append(bb, i); }   // [0]=val, [1]=ptr
    fn.append(bb, ret1(w));

    uint64_t buf[2] = { 5, 0 };
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = reinterpret_cast<uint64_t>(&buf[0]);
    CHECK(jit_vm(fn, px), "jit_vm ok (load/store)");
    CHECK(buf[0] == 105, "buf[0] == 105 (STORE escribio)");
    CHECK(px.regs[0] == 105, "ret == 105 (LOAD leyo)");
    if (buf[0] != 105) std::printf("    buf[0]=%llu\n", (unsigned long long)buf[0]);
}

/* ---- Test 8 (commit 8): ALLOCA host + STORE/LOAD sobre el frame -------- *
 * f(x): p = alloca(8); [p] = x; v = [p]; return v + 100.  f(5) -> 105. */
static void test_vm_alloca() {
    std::printf("[vm] alloca host: [p]=x, load p, +100.  f(5) -> 105\n");
    ir::IrFunction fn;
    fn.name = "al"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId x = fn.new_value(I64);
    ir::IrValueId p = fn.new_value(ir::IrType::PTR);
    ir::IrValueId v = fn.new_value(I64), c = fn.new_value(I64), r = fn.new_value(I64);
    fn.values[p].is_host_ptr = true;   // alloca host -> host_ptr
    fn.params = { x };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr i; i.op = ir::IrOp::ALLOCA; i.type = ir::IrType::I8; i.dst = p;
      i.imm = 8; i.host_alloca = true; fn.append(bb, i); }
    { ir::IrInstr i; i.op = ir::IrOp::STORE; i.type = I64;
      i.operands = { x, p }; fn.append(bb, i); }          // [p] = x
    { ir::IrInstr i; i.op = ir::IrOp::LOAD; i.type = I64; i.dst = v;
      i.operands = { p }; fn.append(bb, i); }             // v = [p]
    fn.append(bb, konst(c, 100));
    fn.append(bb, bin(ir::IrOp::ADD, r, v, c));
    fn.append(bb, ret1(r));

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 5;
    CHECK(jit_vm(fn, px), "jit_vm ok (alloca)");
    CHECK(px.regs[0] == 105, "f(5) == 105 (alloca store/load)");
    if (px.regs[0] != 105) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test 9 (commit 9): TRUNC i32 (sign-ext) y u32 (zero-ext) --------- */
static void test_vm_trunc() {
    std::printf("[vm] trunc.i32(0xFFFFFFFF)=-1 (sign), u32(-1)=0xFFFFFFFF (zero)\n");
    {   /* trunc i64 -> i32 (signed): 0xFFFFFFFF -> -1 sign-extended. */
        ir::IrFunction fn; fn.name = "ti"; fn.ret_type = ir::IrType::I64;
        auto x = fn.new_value(ir::IrType::I64);
        auto t = fn.new_value(ir::IrType::I32);
        fn.params = { x };
        auto bb = fn.new_block("e");
        { ir::IrInstr i; i.op = ir::IrOp::TRUNC; i.type = ir::IrType::I32;
          i.dst = t; i.operands = { x }; fn.append(bb, i); }
        fn.append(bb, ret1(t));
        Proxy px; std::memset(&px, 0, sizeof(px)); px.regs[1] = 0xFFFFFFFFull;
        CHECK(jit_vm(fn, px), "jit_vm ok (trunc i32)");
        CHECK(static_cast<int64_t>(px.regs[0]) == -1,
              "trunc.i32(0xFFFFFFFF) == -1 (sign-ext)");
        if (static_cast<int64_t>(px.regs[0]) != -1)
            std::printf("    regs[0]=0x%llx\n", (unsigned long long)px.regs[0]);
    }
    {   /* trunc i64 -> u32 (unsigned): -1 -> 0xFFFFFFFF zero-extended. */
        ir::IrFunction fn; fn.name = "tu"; fn.ret_type = ir::IrType::I64;
        auto x = fn.new_value(ir::IrType::I64);
        auto t = fn.new_value(ir::IrType::U32);
        fn.params = { x };
        auto bb = fn.new_block("e");
        { ir::IrInstr i; i.op = ir::IrOp::TRUNC; i.type = ir::IrType::U32;
          i.dst = t; i.operands = { x }; fn.append(bb, i); }
        fn.append(bb, ret1(t));
        Proxy px; std::memset(&px, 0, sizeof(px));
        px.regs[1] = 0xFFFFFFFFFFFFFFFFull;
        CHECK(jit_vm(fn, px), "jit_vm ok (trunc u32)");
        CHECK(px.regs[0] == 0xFFFFFFFFull,
              "trunc.u32(-1) == 0xFFFFFFFF (zero-ext)");
        if (px.regs[0] != 0xFFFFFFFFull)
            std::printf("    regs[0]=0x%llx\n", (unsigned long long)px.regs[0]);
    }
}

/** @brief Funcion nativa de prueba (convencion C: args en arg_regs). */
extern "C" uint64_t vm_native_add(uint64_t a, uint64_t b) { return a + b; }

/* ---- Test 10 (commit 10): CALLN directo a funcion nativa --------------- *
 * caller(a,b) = calln "add"(a,b).  CALL DIRECTO (no via runtime vrt_calln).
 * a=10, b=32 -> 42. */
static void test_vm_calln() {
    std::printf("[vm] CALLN directo: add(10,32) -> 42 (sin runtime)\n");
    ir::IrFunction fn;
    fn.name = "cn"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId a = fn.new_value(I64), b = fn.new_value(I64), r = fn.new_value(I64);
    fn.params = { a, b };
    ir::IrBlockId bb = fn.new_block("e");
    {
        ir::IrInstr c; c.op = ir::IrOp::CALLN; c.type = I64; c.dst = r;
        c.func_name = "add"; c.operands = { a, b };
        fn.append(bb, c);
    }
    fn.append(bb, ret1(r));
    CallResolver rn = [](const std::string &n) -> uint64_t {
        return n == "add" ? reinterpret_cast<uint64_t>(
            reinterpret_cast<void *>(&vm_native_add)) : 0;
    };
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 10; px.regs[2] = 32;
    CHECK(jit_vm(fn, px, {}, 0, rn), "jit_vm ok (calln)");
    CHECK(px.regs[0] == 42, "calln add(10,32)==42 (CALL directo)");
    if (px.regs[0] != 42) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test 11 (commit 10): vmath_abs INLINE (sar/xor/sub, sin runtime) ---- */
static void test_vm_abs() {
    std::printf("[vm] vmath_abs INLINE: abs(-5)=5, abs(7)=7 (sin CALL)\n");
    ir::IrFunction fn;
    fn.name = "ab"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId x = fn.new_value(I64), r = fn.new_value(I64);
    fn.params = { x };
    ir::IrBlockId bb = fn.new_block("e");
    {
        ir::IrInstr c; c.op = ir::IrOp::CALLN; c.type = I64; c.dst = r;
        c.func_name = "stdlib/native/math/vesta_math:vmath_abs"; c.operands = { x };
        fn.append(bb, c);
    }
    fn.append(bb, ret1(r));
    /* Sin resolve_native: si NO se inline-ara, CALLN fallaria (resolver={}).
     * Que compile y de el resultado correcto prueba que se inline. */
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = static_cast<uint64_t>(-5);
    CHECK(jit_vm(fn, px), "jit_vm ok (abs inline, sin resolver native)");
    CHECK(static_cast<int64_t>(px.regs[0]) == 5, "abs(-5)==5");
    Proxy px2; std::memset(&px2, 0, sizeof(px2));
    px2.regs[1] = 7;
    jit_vm(fn, px2);
    CHECK(px2.regs[0] == 7, "abs(7)==7");
}

int main() {
    std::setbuf(stdout, nullptr);
    std::printf("=== test_vreg_vm (Phase D.7 commit 5a, VM_ABI) ===\n");
    test_vm_add();
    test_vm_loop();
    test_vm_sext_loop();
    test_vm_call();
    test_vm_callvirt();
    test_vm_gc_stackmap();
    test_vm_load_store();
    test_vm_alloca();
    test_vm_trunc();
    test_vm_calln();
    test_vm_abs();
    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
