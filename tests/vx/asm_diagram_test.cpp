/**
 * @file asm_diagram_test.cpp
 * @brief Tests de los diagramas del CFG del asm anotados con coste
 *        (ver vx/asm_diagram.h): mermaid + graphviz con latencia/cuellos/flags/
 *        diagnosticos.
 */
#include "vx/asm/asm_diagram.h"

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

static bool has(const std::string &s, const std::string &sub) {
    return s.find(sub) != std::string::npos;
}

int main() {
    std::printf("=== asm_diagram_test ===\n");

    int32_t ua = instr_db::microarch_by_name(Isa::X86, "intel-skylake");
    AsmDiagramOptions opt;
    opt.isa = Isa::X86;
    opt.ua_id = static_cast<uint32_t>(ua < 0 ? 0 : ua);
    opt.microarch = "intel-skylake";
    opt.id_prefix = "a0";

    // --- Bloque lineal: subgrafo mermaid con coste + un nodo de bloque. ---
    {
        std::string m = asm_cfg_mermaid("mov rax, rdi\n"
                                        "imul rax, rax\n"
                                        "add rax, rcx\n",
                                        opt);
        CHECK(has(m, "subgraph a0"), "mermaid: subgraph con prefijo");
        CHECK(has(m, "a0_b0"), "mermaid: nodo del bloque 0");
        CHECK(has(m, "end"), "mermaid: cierre del subgraph");
        CHECK(has(m, "lat "), "mermaid: anota latencia");
        CHECK(has(m, "thr "), "mermaid: anota throughput");
        CHECK(has(m, "intel-skylake"), "mermaid: microarq en el titulo");
    }

    // --- if/else: varios bloques + aristas. ---
    {
        std::string m = asm_cfg_mermaid("cmp rax, rbx\n"
                                        "jg .mayor\n"
                                        "mov rcx, 0\n"
                                        "jmp .fin\n"
                                        ".mayor:\n"
                                        "mov rcx, 1\n"
                                        ".fin:\n"
                                        "add rax, rcx\n",
                                        opt);
        CHECK(has(m, "a0_b0 --> ") || has(m, "a0_b0 -.->"),
              "mermaid: arista desde b0");
        CHECK(has(m, "a0_b3"), "mermaid: bloque de merge (b3)");
        CHECK(has(m, "Fw"), "mermaid: marca de escritura de flags (cmp)");
    }

    // --- Bucle infinito: back-edge + diagnostico VXA003 en el diagrama. ---
    {
        std::string m = asm_cfg_mermaid(".loop:\n"
                                        "add rax, 1\n"
                                        "jmp .loop\n",
                                        opt);
        CHECK(has(m, "-.->|back|"), "mermaid: back-edge marcado");
        CHECK(has(m, "VXA003"), "mermaid: diagnostico de bucle en el diagrama");
        CHECK(has(m, "a0_diag"), "mermaid: nodo de diagnosticos");
    }

    // --- graphviz: cluster + nodos + coste. ---
    {
        std::string g = asm_cfg_graphviz("mov rax, rdi\n"
                                         "popcnt rax, rax\n",
                                         opt);
        CHECK(has(g, "subgraph cluster_a0"), "graphviz: cluster con prefijo");
        CHECK(has(g, "a0_b0 [label="), "graphviz: nodo del bloque 0");
        CHECK(has(g, "lat "), "graphviz: anota latencia");
        CHECK(has(g, "label=\""), "graphviz: label del cluster");
    }

    // --- graphviz: bucle -> back-edge dashed + diagnostico. ---
    {
        std::string g = asm_cfg_graphviz(".spin:\n"
                                         "add rax, 1\n"
                                         "jmp .spin\n",
                                         opt);
        CHECK(has(g, "style=dashed"), "graphviz: back-edge dashed");
        CHECK(has(g, "VXA003"), "graphviz: diagnostico de bucle");
    }

    std::printf("=== asm_diagram_test: %d checks OK, %d fallidos ===\n",
                g_checks - g_fail, g_fail);
    return g_fail ? 1 : 0;
}
