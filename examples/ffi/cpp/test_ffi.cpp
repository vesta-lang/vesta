/*
 * test_ffi.cpp -- ejemplo de consumo de libvesta desde C++.
 *
 * Igual que el ejemplo en C pero usando RAII (un wrapper unique_ptr con
 * deleter custom = vesta_free) para que las cadenas/buffers devueltos por
 * la API se liberen automaticamente.  Demuestra el pipeline completo:
 *   eval -> compile_full (vel + ir + velb) -> run -> disasm -> diagram.
 *
 * Compilacion (Windows / MinGW):
 *   g++ -std=c++17 test_ffi.cpp -I../../../include -L../../../cmake-build-release \
 *       -lvesta -o test_ffi.exe
 * Al ejecutar, libvesta.dll (y las DLL de OpenSSL en Windows) deben estar
 * en el PATH o junto al binario.
 *
 * Compilacion (POSIX):
 *   g++ -std=c++17 test_ffi.cpp -I../../../include -L../../../cmake-build-release \
 *       -lvesta -Wl,-rpath,../../../cmake-build-release -o test_ffi
 */

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

#include "capi/vesta.h"

namespace {

// Deleter RAII para buffers/cadenas devueltos por la API (asignados con
// malloc del DLL; se liberan con vesta_free).
struct VestaFree {
    void operator()(void *p) const noexcept { vesta_free(p); }
};
using CStr = std::unique_ptr<char, VestaFree>;
using Bytes = std::unique_ptr<unsigned char, VestaFree>;

// Imprime un fragmento (primeros n chars) de una cadena multilinea.
void print_head(const char *label, const char *text, std::size_t n) {
    std::printf("\n--- %s ---\n", label);
    if (!text) {
        std::printf("(vacio)\n");
        return;
    }
    std::string s(text);
    if (s.size() > n) s = s.substr(0, n) + " [...]";
    std::printf("%s\n", s.c_str());
}

} // namespace

int main() {
    int fallos = 0;
    std::printf("libvesta version: %s\n", vesta_version());

    const char *SRC = "i32 main() {\n"
                      "    println(\"hola desde C++ via FFI\");\n"
                      "    return 6 * 7;\n"
                      "}\n";

    // --- vesta_compile_full: vel + ir + velb en una sola compilacion ---
    {
        char *vel = nullptr, *ir = nullptr, *err = nullptr;
        unsigned char *velb = nullptr;
        std::size_t velb_len = 0;
        int rc = vesta_compile_full(SRC, "cpp_full", &vel, &ir, &velb,
                                    &velb_len, &err);
        CStr vel_g(vel), ir_g(ir), err_g(err);
        Bytes velb_g(velb);
        if (rc != 0) {
            std::fprintf(stderr, "vesta_compile_full fallo (rc=%d): %s\n", rc,
                         err ? err : "(sin mensaje)");
            ++fallos;
        } else {
            print_head("texto .vel (head)", vel, 160);
            print_head("texto IR (head)", ir, 200);
            std::printf("\n.velb generado: %zu bytes\n", velb_len);
            if (velb_len == 0) ++fallos;

            // Ejecutar el .velb producido.
            int exit_val = -1;
            char *rerr = nullptr;
            int rrc = vesta_run(velb, velb_len, 0, nullptr, &exit_val, &rerr);
            CStr rerr_g(rerr);
            if (rrc != 0) {
                std::fprintf(stderr, "vesta_run fallo (rc=%d): %s\n", rrc,
                             rerr ? rerr : "(sin mensaje)");
                ++fallos;
            } else {
                std::printf("vesta_run(.velb) -> exit=%d (esperado 42)\n",
                            exit_val);
                if (exit_val != 42) ++fallos;
            }
        }
    }

    // --- vesta_disasm: desensamblar mov eax,42 ; ret ---
    {
        unsigned char code[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
        char *text = nullptr, *err = nullptr;
        int rc = vesta_disasm(code, sizeof(code), "X86-64", &text, &err);
        CStr text_g(text), err_g(err);
        if (rc != 0) {
            std::fprintf(stderr, "vesta_disasm fallo (rc=%d): %s\n", rc,
                         err ? err : "(sin mensaje)");
            ++fallos;
        } else {
            print_head("desensamblado X86-64", text, 256);
        }
    }

    // --- vesta_diagram: Graphviz (DOT) del AST ---
    {
        char *dot = nullptr, *err = nullptr;
        int rc = vesta_diagram(SRC, "cpp_diag", "ast", "graphviz", &dot, &err);
        CStr dot_g(dot), err_g(err);
        if (rc != 0) {
            std::fprintf(stderr, "vesta_diagram fallo (rc=%d): %s\n", rc,
                         err ? err : "(sin mensaje)");
            ++fallos;
        } else {
            print_head("Graphviz AST (head)", dot, 160);
        }
    }

    if (fallos == 0) {
        std::printf("\nTODOS LOS CASOS OK\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d CASO(S) FALLARON\n", fallos);
    return 1;
}
