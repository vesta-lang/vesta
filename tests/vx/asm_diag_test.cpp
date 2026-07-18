/**
 * @file asm_diag_test.cpp
 * @brief Tests de los diagnosticos estructurales del asm sobre el CFG
 *        (ver vx/asm_diag.h): codigo muerto, salto no resuelto, bucle sin salida.
 */
#include "vx/asm_diag.h"

#include <cstdio>
#include <string>

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

/// ¿Hay algun diagnostico con el codigo @p code?
static bool has_code(const std::vector<AsmDiag> &ds, const std::string &code) {
    for (const AsmDiag &d : ds)
        if (d.code == code)
            return true;
    return false;
}

/// Cuenta diagnosticos con el codigo @p code.
static int count_code(const std::vector<AsmDiag> &ds, const std::string &code) {
    int n = 0;
    for (const AsmDiag &d : ds)
        if (d.code == code)
            ++n;
    return n;
}

int main() {
    std::printf("=== asm_diag_test ===\n");

    // --- Bloque limpio: 0 diagnosticos. ---
    {
        auto ds = asm_diagnose(Isa::X86,
                               "mov rax, rdi\n"
                               "add rax, rsi\n");
        CHECK(ds.empty(), "bloque lineal limpio: 0 diagnosticos");
    }

    // --- Codigo muerto tras un ret. ---
    {
        auto ds = asm_diagnose(Isa::X86,
                               "mov rax, 1\n"
                               "ret\n"
                               "mov rax, 2\n"); // inalcanzable
        CHECK(has_code(ds, "VXA001"), "codigo muerto tras ret -> VXA001");
    }

    // --- Codigo muerto tras un jmp incondicional. ---
    {
        auto ds = asm_diagnose(Isa::X86,
                               "jmp .fin\n"
                               "mov rax, 7\n" // inalcanzable
                               ".fin:\n"
                               "mov rax, 0\n");
        CHECK(has_code(ds, "VXA001"), "codigo muerto tras jmp -> VXA001");
    }

    // --- Salto a etiqueta no definida. ---
    {
        auto ds = asm_diagnose(Isa::X86,
                               "cmp rax, 0\n"
                               "je .noexiste\n"
                               "mov rax, 1\n");
        CHECK(has_code(ds, "VXA002"), "salto a etiqueta externa -> VXA002");
    }

    // --- Bucle sin salida (infinito). ---
    {
        auto ds = asm_diagnose(Isa::X86,
                               ".loop:\n"
                               "add rax, 1\n"
                               "jmp .loop\n"); // nunca sale
        CHECK(has_code(ds, "VXA003"), "bucle infinito -> VXA003");
        CHECK(count_code(ds, "VXA003") == 1, "bucle infinito: un solo aviso");
    }

    // --- Bucle CON salida: no debe reportar VXA003. ---
    {
        auto ds = asm_diagnose(Isa::X86,
                               "mov rcx, 10\n"
                               ".loop:\n"
                               "dec rcx\n"
                               "jnz .loop\n"); // sale por fallthrough al final
        CHECK(!has_code(ds, "VXA003"), "bucle con salida: sin VXA003");
    }

    // --- arm64: bucle infinito con b incondicional. ---
    {
        auto ds = asm_diagnose(Isa::ARM64,
                               ".spin:\n"
                               "add x0, x0, #1\n"
                               "b .spin\n");
        CHECK(has_code(ds, "VXA003"), "arm64 bucle infinito -> VXA003");
    }

    // --- Salto indirecto: NO se reporta bucle falso (conservador). ---
    {
        auto ds = asm_diagnose(Isa::X86,
                               ".loop:\n"
                               "add rax, 1\n"
                               "jmp rax\n"); // indirecto: podria salir
        CHECK(!has_code(ds, "VXA003"),
              "salto indirecto: sin bucle falso (conservador)");
    }

    std::printf("=== asm_diag_test: %d checks OK, %d fallidos ===\n",
                g_checks - g_fail, g_fail);
    return g_fail ? 1 : 0;
}
