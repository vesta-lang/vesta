/**
 * @file asm_lift_test.cpp
 * @brief Tests del reconocedor de patrones de asm liftables a IR tipado
 *        (ver vx/asm_lift.h): lock cmpxchg -> ATOMIC_CAS, lock xadd ->
 * ATOMIC_ADD.
 */
#include "vx/asm/asm_lift.h"

#include <cstdio>

using namespace vx;
using vx::instr_db::Isa;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fail;                                                          \
            std::printf("  FAIL: %s (linea %d)\n", (msg), __LINE__);           \
        }                                                                      \
    } while (0)

int main() {
    std::printf("=== asm_lift_test ===\n");

    // --- lock cmpxchg [rdi], rsi -> ATOMIC_CAS (addr=rdi, des=rsi,
    // exp/old=rax). ---
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
        AsmLift l =
            asm_lift_detect(Isa::X86, ".cas:\n"
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

    // --- Multi-instruccion: `mov rax, exp` + `lock cmpxchg` -> AtomicCas con
    //     el expected EXPLICITO (no el rax implicito). ---
    {
        AsmLift l = asm_lift_detect(Isa::X86, "mov rax, rdx\n"
                                              "lock cmpxchg [rdi], rsi\n");
        CHECK(l.op == AsmLiftOp::AtomicCas, "multi mov+cmpxchg -> AtomicCas");
        CHECK(l.addr_reg == "rdi", "multi: addr=rdi");
        CHECK(l.exp_reg == "rdx", "multi: exp=rdx (EXPLICITO)");
        CHECK(l.des_reg == "rsi", "multi: des=rsi");
        CHECK(l.result_reg == "rax", "multi: old=rax");
    }
    // --- Instruccion intermedia que NO toca rax: sigue liftando. ---
    {
        AsmLift l = asm_lift_detect(Isa::X86, "mov rax, rdx\n"
                                              "add rbx, 1\n"
                                              "lock cmpxchg [rdi], rsi\n");
        CHECK(l.op == AsmLiftOp::AtomicCas, "multi con intermedia inocua");
        CHECK(l.exp_reg == "rdx", "multi: exp=rdx pese a la intermedia");
    }
    // --- Instruccion intermedia que PISA rax: NO se lifta (efectos). ---
    {
        AsmLift l = asm_lift_detect(Isa::X86, "mov rax, rdx\n"
                                              "xor rax, rax\n"
                                              "lock cmpxchg [rdi], rsi\n");
        CHECK(l.op == AsmLiftOp::None, "multi: intermedia pisa rax -> None");
    }

    // --- Otro mnemonico: NO se lifta. ---
    {
        AsmLift l = asm_lift_detect(Isa::X86, "add rax, rbx\n");
        CHECK(l.op == AsmLiftOp::None, "add -> None");
    }

    // === arm64: bucle load-linked / store-conditional -> ATOMIC_CAS. ===

    // --- Bucle CAS canonico ldaxr/cmp/b.ne/stlxr/cbnz. ---
    {
        AsmLift l = asm_lift_detect(Isa::ARM64, ".retry:\n"
                                                "  ldaxr x0, [x1]\n"
                                                "  cmp x0, x2\n"
                                                "  b.ne .done\n"
                                                "  stlxr w3, x4, [x1]\n"
                                                "  cbnz w3, .retry\n"
                                                ".done:\n"
                                                "  ret\n");
        // Nota: el .done + ret hacen 7 instrucciones; el bucle son 5 hasta
        // cbnz. El reconocedor exige EXACTAMENTE el bucle de 5, asi que este
        // caso con cola NO encaja -> None (conservador).
        CHECK(l.op == AsmLiftOp::None,
              "arm64 con cola tras cbnz -> None (inc.3)");
    }

    // --- Bucle CAS exacto (5 instrucciones, .done al final sin cuerpo). ---
    {
        AsmLift l = asm_lift_detect(Isa::ARM64, ".retry:\n"
                                                "  ldaxr x0, [x1]\n"
                                                "  cmp x0, x2\n"
                                                "  b.ne .done\n"
                                                "  stlxr w3, x4, [x1]\n"
                                                "  cbnz w3, .retry\n");
        CHECK(l.op == AsmLiftOp::AtomicCas, "arm64 LL/SC -> AtomicCas");
        CHECK(l.addr_reg == "x1", "arm64: addr=x1");
        CHECK(l.exp_reg == "x2", "arm64: exp=x2");
        CHECK(l.des_reg == "x4", "arm64: des=x4");
        CHECK(l.result_reg == "x0", "arm64: old=x0");
    }

    // --- Variante relajada ldxr/stxr: tambien se reconoce. ---
    {
        AsmLift l = asm_lift_detect(Isa::ARM64, ".r:\n"
                                                "  ldxr x5, [x6]\n"
                                                "  cmp x5, x7\n"
                                                "  b.ne .e\n"
                                                "  stxr w8, x9, [x6]\n"
                                                "  cbnz w8, .r\n");
        CHECK(l.op == AsmLiftOp::AtomicCas, "arm64 ldxr/stxr -> AtomicCas");
        CHECK(l.addr_reg == "x6" && l.result_reg == "x5",
              "arm64: addr=x6 old=x5");
    }

    // --- Direccion inconsistente entre ldaxr y stlxr: NO se lifta. ---
    {
        AsmLift l = asm_lift_detect(Isa::ARM64,
                                    ".r:\n"
                                    "  ldaxr x0, [x1]\n"
                                    "  cmp x0, x2\n"
                                    "  b.ne .e\n"
                                    "  stlxr w3, x4, [x9]\n" // [x9] != [x1]
                                    "  cbnz w3, .r\n");
        CHECK(l.op == AsmLiftOp::None,
              "arm64: direccion inconsistente -> None");
    }

    // --- El cbnz no vuelve al ldaxr: NO es el bucle. ---
    {
        AsmLift l = asm_lift_detect(Isa::ARM64,
                                    ".r:\n"
                                    "  ldaxr x0, [x1]\n"
                                    "  cmp x0, x2\n"
                                    "  b.ne .e\n"
                                    "  stlxr w3, x4, [x1]\n"
                                    "  cbnz w3, .otro\n"); // no vuelve a .r
        CHECK(l.op == AsmLiftOp::None,
              "arm64: cbnz no vuelve al ldaxr -> None");
    }

    // --- 32 bits (registros w): NO se lifta (solo i64). ---
    {
        AsmLift l = asm_lift_detect(Isa::ARM64, ".r:\n"
                                                "  ldaxr w0, [x1]\n"
                                                "  cmp w0, w2\n"
                                                "  b.ne .e\n"
                                                "  stlxr w3, w4, [x1]\n"
                                                "  cbnz w3, .r\n");
        CHECK(l.op == AsmLiftOp::None, "arm64 32-bit -> None (solo i64)");
    }

    std::printf("=== asm_lift_test: %d checks OK, %d fallidos ===\n",
                g_checks - g_fail, g_fail);
    return g_fail ? 1 : 0;
}
