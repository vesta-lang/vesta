/*
 * test_ffi.c -- ejemplo de consumo de libvesta desde C puro.
 *
 * Demuestra las tres rutas de la API:
 *   1. vesta_eval  : compila + ejecuta una fuente Vex en una llamada.
 *   2. vesta_compile + vesta_run : compila a bytecode .velb y luego lo ejecuta.
 *
 * Compilacion (Windows / MinGW):
 *   gcc test_ffi.c -I../../../include -L<build_dir> -lvesta -o test_ffi
 * donde <build_dir> es el directorio con libvesta.dll y libvesta.dll.a
 * (p.ej. cmake-build-release).  Al ejecutar, libvesta.dll y las DLL de
 * OpenSSL (libssl-3-x64.dll, libcrypto-3-x64.dll) deben estar en el PATH
 * o junto al ejecutable.
 *
 * Compilacion (POSIX):
 *   gcc test_ffi.c -I../../../include -L<build_dir> -lvesta \
 *       -Wl,-rpath,<build_dir> -o test_ffi
 */

#include <stdio.h>
#include <stdlib.h>

#include "capi/vesta.h"

/* Programa Vex de prueba: main devuelve 42. */
static const char *SRC_42 = "i32 main() { return 42; }\n";

/* Programa Vex de prueba: main devuelve 6 * 7 = 42 con un println. */
static const char *SRC_MUL =
    "i32 main() {\n"
    "    println(\"hola desde Vex via FFI\");\n"
    "    return 6 * 7;\n"
    "}\n";

int main(void) {
    int fallos = 0;

    printf("libvesta version: %s\n", vesta_version());

    /* --- Caso 1: vesta_eval (compile + run) --- */
    {
        int exit_val = -1;
        char *err = NULL;
        int rc = vesta_eval(SRC_42, "test_42", &exit_val, &err);
        if (rc != 0) {
            fprintf(stderr, "vesta_eval fallo (rc=%d): %s\n", rc,
                    err ? err : "(sin mensaje)");
            vesta_free(err);
            fallos++;
        } else {
            printf("vesta_eval(SRC_42) -> exit=%d (esperado 42)\n", exit_val);
            if (exit_val != 42) fallos++;
        }
    }

    /* --- Caso 2: vesta_compile + vesta_run por separado --- */
    {
        unsigned char *velb = NULL;
        size_t velb_len = 0;
        char *err = NULL;
        int rc = vesta_compile(SRC_MUL, "test_mul", &velb, &velb_len, &err);
        if (rc != 0) {
            fprintf(stderr, "vesta_compile fallo (rc=%d): %s\n", rc,
                    err ? err : "(sin mensaje)");
            vesta_free(err);
            fallos++;
        } else {
            printf("vesta_compile(SRC_MUL) -> %zu bytes de .velb\n", velb_len);
            if (velb_len == 0) fallos++;

            int exit_val = -1;
            char *rerr = NULL;
            int rrc = vesta_run(velb, velb_len, 0, NULL, &exit_val, &rerr);
            if (rrc != 0) {
                fprintf(stderr, "vesta_run fallo (rc=%d): %s\n", rrc,
                        rerr ? rerr : "(sin mensaje)");
                vesta_free(rerr);
                fallos++;
            } else {
                printf("vesta_run(.velb) -> exit=%d (esperado 42)\n", exit_val);
                if (exit_val != 42) fallos++;
            }
            vesta_free(velb);
        }
    }

    /* --- Caso 3: vesta_compile_to_vel (Vex -> texto .vel) --- */
    {
        char *vel = NULL, *err = NULL;
        int rc = vesta_compile_to_vel(SRC_42, "c_vel", &vel, &err);
        if (rc != 0) {
            fprintf(stderr, "vesta_compile_to_vel fallo (rc=%d): %s\n", rc,
                    err ? err : "(sin mensaje)");
            vesta_free(err);
            fallos++;
        } else {
            printf("\n--- .vel de SRC_42 (primeras lineas) ---\n");
            /* Imprimir solo un fragmento para no saturar la salida. */
            printf("%.200s%s\n", vel,
                   (vel && vel[0] && vel[199]) ? " [...]" : "");
            vesta_free(vel);
        }
    }

    /* --- Caso 4: vesta_compile_to_ir (Vex -> texto IR SSA) --- */
    {
        char *ir = NULL, *err = NULL;
        int rc = vesta_compile_to_ir(SRC_MUL, "c_ir", &ir, &err);
        if (rc != 0) {
            fprintf(stderr, "vesta_compile_to_ir fallo (rc=%d): %s\n", rc,
                    err ? err : "(sin mensaje)");
            vesta_free(err);
            fallos++;
        } else {
            printf("\n--- IR de SRC_MUL (primeras lineas) ---\n");
            printf("%.300s%s\n", ir,
                   (ir && ir[0] && ir[299]) ? " [...]" : "");
            vesta_free(ir);
        }
    }

    /* --- Caso 5: vesta_assemble (texto .vel -> bytes .velb) --- */
    {
        char *vel = NULL, *verr = NULL;
        if (vesta_compile_to_vel(SRC_42, "c_asm", &vel, &verr) == 0) {
            unsigned char *velb = NULL;
            size_t velb_len = 0;
            char *aerr = NULL;
            int rc = vesta_assemble(vel, &velb, &velb_len, &aerr);
            if (rc != 0) {
                fprintf(stderr, "vesta_assemble fallo (rc=%d): %s\n", rc,
                        aerr ? aerr : "(sin mensaje)");
                vesta_free(aerr);
                fallos++;
            } else {
                printf("\nvesta_assemble(.vel) -> %zu bytes de .velb\n",
                       velb_len);
                if (velb_len == 0) fallos++;
                vesta_free(velb);
            }
            vesta_free(vel);
        } else {
            vesta_free(verr);
            fallos++;
        }
    }

    /* --- Caso 6: vesta_disasm (desensamblar un buffer x86-64) --- */
    {
        /* mov eax, 42 ; ret  -> debe desensamblarse a "mov" + "ret". */
        unsigned char code[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
        char *text = NULL, *err = NULL;
        int rc = vesta_disasm(code, sizeof(code), "X86-64", &text, &err);
        if (rc != 0) {
            fprintf(stderr, "vesta_disasm fallo (rc=%d): %s\n", rc,
                    err ? err : "(sin mensaje)");
            vesta_free(err);
            fallos++;
        } else {
            printf("\n--- desensamblado de [B8 2A 00 00 00 C3] ---\n%s", text);
            vesta_free(text);
        }
    }

    /* --- Caso 7: vesta_diagram (Mermaid del IR post-opt) --- */
    {
        char *diag = NULL, *err = NULL;
        int rc = vesta_diagram(SRC_MUL, "c_diag", "ir-post", "mermaid", &diag,
                               &err);
        if (rc != 0) {
            fprintf(stderr, "vesta_diagram fallo (rc=%d): %s\n", rc,
                    err ? err : "(sin mensaje)");
            vesta_free(err);
            fallos++;
        } else {
            printf("\n--- diagrama Mermaid (ir-post) primeras lineas ---\n");
            printf("%.200s%s\n", diag,
                   (diag && diag[0] && diag[199]) ? " [...]" : "");
            vesta_free(diag);
        }
    }

    if (fallos == 0) {
        printf("\nTODOS LOS CASOS OK\n");
        return 0;
    }
    fprintf(stderr, "\n%d CASO(S) FALLARON\n", fallos);
    return 1;
}
