/**
 * @file asm_lift_test.cpp
 * @brief Tests del reconocedor de patrones de asm liftables a IR tipado
 *        (ver vx/asm_lift.h): lock cmpxchg -> ATOMIC_CAS, lock xadd -> ATOMIC_ADD.
 */
#include "vx/asm_lift.h"

#include <cstdio>

using namespace vx;
using vx::instr_db::Isa;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_fail;                                                           \
            std::printf("  FAIL: %s (linea %d)\n", (msg), __LINE__);            \
        }                                                                       \
    } while (0)

int main() {
    std::printf("=== asm_lift_test ===\n");

    // --- lock cmpxchg [rdi], rsi -> ATOMIC_CAS (addr=rdi, des=rsi, exp/old=rax). ---
    {
        AsmLift l = asm_lift_detect(Isa::X86, "lock cmpxchg [rdi], rsi\n");
        CHECK(l.op == AsmLiftOp::AtomicCas, "cmpxchg -> AtomicCas");
        CHECK(l.addr_reg == "rdi", "cmpxchg: addr=rdi");
        CHECK(l.des_reg == "rsi", "cmpxchg: des=rsi");
        CHECK(l.exp_reg == "rax", "cmpxchg: exp=rax (implicito)");
        CHECK(l.result_reg == "rax", "cmpxchg: old=rax");
        CHECK(l.width == 64, "cmpxchg: width 64");
    }

    // --- Con etiqueta y comentario alrededor: sigue liftando. ---
    {
        AsmLift l = asm_lift_detect(Isa::X86,
                                    ".cas:\n"
                                    "  lock cmpxchg [r8], r9  ; CAS\n");
        CHECK(l.op == AsmLiftOp::AtomicCas, "cmpxchg con label/comentario");
        CHECK(l.addr_reg == "r8" && l.des_reg == "r9", "addr=r8 des=r9");
    }

    // --- lock xadd [rdi], rsi -> ATOMIC_ADD (delta=rsi, old=rsi). ---
    {
        AsmLift l = asm_lift_detect(Isa::X86, "lock xadd [rcx], rdx\n");
        CHECK(l.op == AsmLiftOp::AtomicAdd, "xadd -> AtomicAdd");
        CHECK(l.addr_reg == "rcx", "xadd: addr=rcx");
        CHECK(l.des_reg == "rdx", "xadd: delta=rdx");
        CHECK(l.result_reg == "rdx", "xadd: old queda en rdx");
    }

    // --- Sin `lock`: NO se lifta (no es atomico). ---
    {
        AsmLift l = asm_lift_detect(Isa::X86, "cmpxchg [rdi], rsi\n");
        CHECK(l.op == AsmLiftOp::None, "cmpxchg sin lock -> None");
    }

    // --- 32 bits: NO se lifta (solo i64 por ahora). ---
    {
        AsmLift l = asm_lift_detect(Isa::X86, "lock cmpxchg [rdi], esi\n");
        CHECK(l.op == AsmLiftOp::None, "cmpxchg 32-bit -> None (solo i64)");
    }

    // --- Memoria con desplazamiento: NO se lifta (solo [reg] plano). ---
    {
        AsmLift l = asm_lift_detect(Isa::X86, "lock cmpxchg [rdi+8], rsi\n");
        CHECK(l.op == AsmLiftOp::None, "cmpxchg [rdi+8] -> None (solo [reg])");
    }

    // --- Multiples instrucciones: NO se lifta (forma de 1 instr en inc.1). ---
    {
        AsmLift l = asm_lift_detect(Isa::X86,
                                    "mov rax, rdx\n"
                                    "lock cmpxchg [rdi], rsi\n");
        CHECK(l.op == AsmLiftOp::None, "multi-instr -> None (inc.1)");
    }

    // --- Otro mnemonico: NO se lifta. ---
    {
        AsmLift l = asm_lift_detect(Isa::X86, "add rax, rbx\n");
        CHECK(l.op == AsmLiftOp::None, "add -> None");
    }

    std::printf("=== asm_lift_test: %d checks OK, %d fallidos ===\n",
                g_checks - g_fail, g_fail);
    return g_fail ? 1 : 0;
}
