/**
 * @file test_asm_analyze.cpp
 * @brief Tests del resumen de efectos de bloque de un cuerpo de inline asm.
 *        Ver vx/asm_analyze.h.  Llama a la API C++ directamente -- el mismo uso
 *        que hara el servidor de lenguaje (linkado contra el compilador).
 *
 * Cubre las cuatro arquitecturas: x86_64, x86 (32-bit), x86_16 y arm64.
 */
#include "vx/asm/asm_analyze.h"

#include <cstdio>
#include <string>

using namespace vx;

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
    // --- x86-64: lock cmpxchg (CAS atomico) ------------------------------
    {
        AsmBlockEffects e =
            asm_analyze_block("  mov rax, rsi\n  lock cmpxchg [rdi], rdx\n",
                              "x86_64");
        CHECK(e.known(), "x86_64 cas: mnemonicos conocidos");
        CHECK(e.has_atomic, "x86_64 cas: lock -> atomica");
        CHECK(e.touches_mem, "x86_64 cas: toca memoria");
        CHECK(!e.is_call, "x86_64 cas: sin call");
        CHECK(e.explicit_stack_bytes == 0, "x86_64 cas: sin marco explicito");
    }

    // --- x86-64/32/16: el mismo push mide 8/4/2 bytes --------------------
    {
        const char *body = "  push rbp\n";
        CHECK(asm_analyze_block(body, "x86_64").explicit_stack_bytes == 8,
              "x86_64: push = 8B");
        CHECK(asm_analyze_block(body, "x86").explicit_stack_bytes == 4,
              "x86-32: push = 4B");
        CHECK(asm_analyze_block(body, "x86_16").explicit_stack_bytes == 2,
              "x86_16: push = 2B");
    }

    // --- x86: prologo con marco explicito (pico, no neto) ----------------
    {
        AsmBlockEffects e = asm_analyze_block(
            "  push rbp\n  sub rsp, 32\n  mov [rsp], rax\n  add rsp, 32\n"
            "  pop rbp\n",
            "x86_64");
        CHECK(e.known(), "x86 prologo: conocido");
        CHECK(e.explicit_stack_bytes == 40, "x86 prologo: marco pico = 40");
        CHECK(e.touches_mem, "x86 prologo: toca memoria");
    }

    // --- x86: rama + flags -----------------------------------------------
    {
        AsmBlockEffects e =
            asm_analyze_block("  cmp rax, rbx\n  jne .otro\n.otro:\n", "x86_64");
        CHECK(e.has_branch, "x86 rama: jne -> rama");
        CHECK(e.touches_flags, "x86 rama: cmp -> flags");
        CHECK(!e.touches_mem, "x86 rama: sin memoria");
    }

    // --- arm64: bucle LL/SC (ldaxr/stlxr) TODO reconocido ----------------
    {
        AsmBlockEffects e = asm_analyze_block(
            ".retry:\n  ldaxr x3, [x0]\n  cmp x3, x1\n  b.ne .done\n"
            "  stlxr w4, x2, [x0]\n  cbnz w4, .retry\n.done:\n",
            "arm64");
        CHECK(e.known(), "arm64 cas: TODO reconocido (0 desconocidos)");
        CHECK(e.has_atomic, "arm64 cas: ldaxr/stlxr -> atomica");
        CHECK(e.touches_mem, "arm64 cas: [x0] -> memoria");
        CHECK(e.has_branch, "arm64 cas: b.ne/cbnz -> rama");
        CHECK(e.touches_flags, "arm64 cas: cmp -> flags");
        CHECK(e.explicit_stack_bytes == 0, "arm64 cas: sin marco explicito");
    }

    // --- arm64: CAS de armv8.1 + marco explicito sub sp ------------------
    {
        AsmBlockEffects e = asm_analyze_block(
            "  sub sp, sp, #16\n  casal x0, x1, [x2]\n  add sp, sp, #16\n",
            "arm64");
        CHECK(e.known(), "arm64 casal: reconocido");
        CHECK(e.has_atomic, "arm64 casal: -> atomica");
        CHECK(e.touches_mem, "arm64 casal: -> memoria");
        CHECK(e.explicit_stack_bytes == 16, "arm64: sub sp,#16 -> 16B");
    }

    // --- arm64: un mnemonico x86 NO existe en arm64 (error claro) --------
    {
        AsmBlockEffects e = asm_analyze_block("  cpuid\n", "arm64");
        CHECK(!e.known(), "arm64: cpuid no es de arm64 -> desconocido");
        CHECK(e.unknown_mnemonics == std::vector<std::string>{"cpuid"},
              "arm64: nombra cpuid");
    }

    // --- desconocido en x86: error claro ---------------------------------
    {
        AsmBlockEffects e =
            asm_analyze_block("  chorradadesconocida rax, rbx\n", "x86_64");
        CHECK(!e.known(), "desconocido: known()==false");
        CHECK(e.unknown_mnemonics ==
                  std::vector<std::string>{"chorradadesconocida"},
              "desconocido: nombra el mnemonico exacto");
    }

    if (g_fail == 0)
        std::printf("=== test_asm_analyze: %d checks OK, 0 fallidos ===\n",
                    g_checks);
    else
        std::printf("=== test_asm_analyze: %d checks, %d FALLIDOS ===\n",
                    g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
