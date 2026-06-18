/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_asm_trampoline.cpp
 * @brief Phase AS inc.6: test del trampoline de inline-asm para el interprete.
 *        Construye trampolines reales (Keystone) y los EJECUTA con un array
 *        ctx[16], verificando el marshalling de registros host <-> ctx.
 *        Valida ademas que un asm que pisa rbx/rbp (cpuid) NO corrompe el
 *        trampoline (ctx vive en la pila).
 */

#include "jit/inline_asm_trampoline.h"
#include "jit/code_cache.h"
#include "jit/keystone_asm_backend.h"

#include <cstdint>
#include <cstdio>

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

/* ids de registro GP host en ctx: rax=0 rcx=1 rdx=2 rbx=3 rsp=4 rbp=5
 * rsi=6 rdi=7 r8=8 .. r15=15 */
enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RBP = 5, RSI = 6, RDI = 7 };

int main() {
    std::printf("=== test_asm_trampoline (Phase AS inc.6) ===\n");
    register_keystone_asm_backend(); // instala g_asm_backend (Keystone)
    CodeCache cc;

    /* --- Test 0 (diag): mov rax, rdi -- aisla el load de rdi --- */
    {
        AsmTrampolineFn fn = build_asm_trampoline("mov rax, rdi", cc, nullptr);
        if (fn) {
            uint64_t ctx[16] = {0};
            ctx[RDI] = 0x1234;
            fn(ctx);
            std::printf(
                "  [diag] mov rax,rdi: ctx[rdi]=0x1234 -> ctx[rax]=0x%llx\n",
                (unsigned long long)ctx[RAX]);
            CHECK(ctx[RAX] == 0x1234, "diag: load rdi + mov rax,rdi");
        }
    }

    /* --- Test 1: popcnt rax, rdi (input rdi, output rax) --- */
    {
        std::string err;
        AsmTrampolineFn fn = build_asm_trampoline("popcnt rax, rdi", cc, &err);
        CHECK(fn != nullptr, "popcnt: trampoline construido");
        if (fn) {
            uint64_t ctx[16] = {0};
            ctx[RDI] = 0xFF; // 8 bits a 1
            fn(ctx);
            std::printf("  popcnt(0xFF) -> ctx[rax] = %llu\n",
                        (unsigned long long)ctx[RAX]);
            CHECK(ctx[RAX] == 8, "popcnt(0xFF) == 8");
        } else {
            std::printf("  err: %s\n", err.c_str());
        }
    }

    /* --- Test 2: add rax, rax (inout rax) --- */
    {
        AsmTrampolineFn fn = build_asm_trampoline("add rax, rax", cc, nullptr);
        CHECK(fn != nullptr, "add: trampoline construido");
        if (fn) {
            uint64_t ctx[16] = {0};
            ctx[RAX] = 21;
            fn(ctx);
            std::printf("  double(21) -> ctx[rax] = %llu\n",
                        (unsigned long long)ctx[RAX]);
            CHECK(ctx[RAX] == 42, "double(21) == 42");
        }
    }

    /* --- Test 3: lea rax, [rdi + rsi] (2 inputs, 1 output) --- */
    {
        AsmTrampolineFn fn =
            build_asm_trampoline("lea rax, [rdi + rsi]", cc, nullptr);
        CHECK(fn != nullptr, "lea: trampoline construido");
        if (fn) {
            uint64_t ctx[16] = {0};
            ctx[RDI] = 40;
            ctx[RSI] = 2;
            fn(ctx);
            CHECK(ctx[RAX] == 42, "lea(40,2) == 42");
        }
    }

    /* --- Test 4: cpuid (pisa rbx=ProcessVM* en el JIT; aqui pisa rbx que el
     * trampoline carga de ctx -- el ctx en pila debe sobrevivir + retornar
     * sin corromper el stack). --- */
    {
        AsmTrampolineFn fn = build_asm_trampoline("cpuid", cc, nullptr);
        CHECK(fn != nullptr, "cpuid: trampoline construido");
        if (fn) {
            uint64_t ctx[16] = {0};
            ctx[RAX] = 0; // leaf 0 -> max basic leaf en eax
            /* marcador en un reg no tocado por cpuid para verificar que el
             * marshalling + el ret no corrompen el resto. */
            ctx[RSI] = 0xCAFEBABE;
            fn(ctx);
            std::printf(
                "  cpuid(0): ctx[rax]=max_leaf=%llu ctx[rbx]=vendor=0x%llx\n",
                (unsigned long long)ctx[RAX], (unsigned long long)ctx[RBX]);
            CHECK(ctx[RAX] != 0, "cpuid(0) max leaf != 0 (ejecuto)");
            CHECK(ctx[RBX] != 0, "cpuid(0) ebx (vendor) != 0 (rbx clobber OK)");
            CHECK(ctx[RSI] == 0xCAFEBABE, "rsi no tocado preservado (ret OK)");
        }
    }

    std::printf("\n%d checks, %d fallos\n", g_checks, g_fails);
    asm volatile("" : : "r"(&cc) : "memory"); // mantener cc viva
    return g_fails == 0 ? 0 : 1;
}
