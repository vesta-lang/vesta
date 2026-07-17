/**
 * @file test_asm_analyze.cpp
 * @brief Tests del resumen de efectos de bloque de un cuerpo de inline asm.
 *        Ver vx/asm_analyze.h.  Llama a la API C++ directamente -- el mismo uso
 *        que hara el servidor de lenguaje (linkado contra el compilador).
 */
#include "vx/asm_analyze.h"

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
    // --- x86: lock cmpxchg (CAS atomico) ---------------------------------
    {
        const std::string body =
            "  mov rax, rsi\n"
            "  lock cmpxchg [rdi], rdx\n";
        AsmBlockEffects e = asm_analyze_block(body, "x86_64");
        CHECK(e.known(), "x86 cas: todos los mnemonicos conocidos");
        CHECK(e.has_atomic, "x86 cas: lock -> atomica");
        CHECK(e.touches_mem, "x86 cas: toca memoria ([rdi])");
        CHECK(!e.is_call, "x86 cas: no hace call");
        CHECK(e.explicit_stack_bytes == 0, "x86 cas: sin marco explicito");
    }

    // --- x86: prologo con marco explicito (pico, no neto) ----------------
    {
        const std::string body =
            "  push rbp\n"
            "  sub rsp, 32\n"
            "  mov [rsp], rax\n"
            "  add rsp, 32\n"
            "  pop rbp\n";
        AsmBlockEffects e = asm_analyze_block(body, "x86_64");
        CHECK(e.known(), "x86 prologo: conocidos");
        // push rbp (8) + sub rsp,32 -> pico 40; add/pop lo liberan.
        CHECK(e.explicit_stack_bytes == 40,
              "x86 prologo: marco pico = 40 (peor caso, no neto)");
        CHECK(e.touches_mem, "x86 prologo: toca memoria");
    }

    // --- x86: rama detectada ---------------------------------------------
    {
        const std::string body =
            "  cmp rax, rbx\n"
            "  jne .otro\n"
            ".otro:\n";
        AsmBlockEffects e = asm_analyze_block(body, "x86_64");
        CHECK(e.known(), "x86 rama: conocidos");
        CHECK(e.has_branch, "x86 rama: jne -> rama");
        CHECK(e.touches_flags, "x86 rama: cmp -> flags");
        CHECK(!e.touches_mem, "x86 rama: no toca memoria");
    }

    // --- arm64: bucle LL/SC (ldaxr/stlxr) --------------------------------
    {
        const std::string body =
            ".retry:\n"
            "  ldaxr x3, [x0]\n"
            "  cmp x3, x1\n"
            "  b.ne .done\n"
            "  stlxr w4, x2, [x0]\n"
            "  cbnz w4, .retry\n"
            ".done:\n";
        AsmBlockEffects e = asm_analyze_block(body, "arm64");
        CHECK(e.has_atomic, "arm64 cas: ldaxr/stlxr -> atomica");
        CHECK(e.touches_mem, "arm64 cas: [x0] -> memoria");
        CHECK(e.has_branch, "arm64 cas: b.ne/cbnz -> rama");
        CHECK(e.explicit_stack_bytes == 0, "arm64 cas: sin marco explicito");
        for (const auto &u : e.unknown_mnemonics)
            CHECK(u != "ldaxr" && u != "stlxr",
                  "arm64 cas: LL/SC no cuentan como desconocidas");
    }

    // --- desconocido: error claro (nombra el mnemonico) ------------------
    {
        const std::string body = "  chorradadesconocida rax, rbx\n";
        AsmBlockEffects e = asm_analyze_block(body, "x86_64");
        CHECK(!e.known(), "desconocido: known()==false");
        CHECK(e.unknown_mnemonics.size() == 1 &&
                  e.unknown_mnemonics[0] == "chorradadesconocida",
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
