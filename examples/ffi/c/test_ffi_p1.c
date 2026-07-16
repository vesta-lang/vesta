/*
 * test_ffi_p1.c -- ejemplo de las 4 funciones P1 de libvesta desde C puro.
 *
 * Demuestra:
 *   1. vesta_ir_to_velb : texto IR SSA -> bytecode .velb -> ejecucion.
 *                         Cierra el ciclo compile_to_ir -> ir_to_velb -> run.
 *   2. vesta_json_validate : valida JSON correcto e incorrecto.
 *   3. vesta_json_format : minifica y embellece JSON.
 *   4. vesta_sqlite_exec : ejecuta SQL sobre una BD ":memory:" y obtiene JSON.
 *
 * Compilacion (Windows / MinGW):
 *   gcc test_ffi_p1.c -I../../../include -L<build_dir> -lvesta -o test_ffi_p1
 * donde <build_dir> es el directorio con libvesta.dll y libvesta.dll.a
 * (p.ej. cmake-build-release).  Al ejecutar, libvesta.dll y las DLL de
 * OpenSSL (libssl-3-x64.dll, libcrypto-3-x64.dll) deben estar en el PATH
 * o junto al ejecutable.
 *
 * Compilacion (POSIX):
 *   gcc test_ffi_p1.c -I../../../include -L<build_dir> -lvesta \
 *       -Wl,-rpath,<build_dir> -o test_ffi_p1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capi/vesta.h"

/* Fuente Vesta-lang cuyo IR se obtiene y luego se reconvierte a .velb. */
static const char *SRC_7 = "i32 main() { return 7; }\n";

int main(void) {
    int fallos = 0;

    printf("libvesta version: %s\n", vesta_version());

    /* --- Caso 1: vesta_compile_to_ir -> vesta_ir_to_velb -> vesta_run --- */
    {
        char *ir = NULL, *err = NULL;
        int rc = vesta_compile_to_ir(SRC_7, "p1_ir", &ir, &err);
        if (rc != 0) {
            fprintf(stderr, "vesta_compile_to_ir fallo (rc=%d): %s\n", rc,
                    err ? err : "(sin mensaje)");
            vesta_free(err);
            fallos++;
        } else {
            unsigned char *velb = NULL;
            size_t len = 0;
            char *err2 = NULL;
            rc = vesta_ir_to_velb(ir, &velb, &len, &err2);
            if (rc != 0) {
                fprintf(stderr, "vesta_ir_to_velb fallo (rc=%d): %s\n", rc,
                        err2 ? err2 : "(sin mensaje)");
                vesta_free(err2);
                fallos++;
            } else {
                int code = -1;
                char *err3 = NULL;
                rc = vesta_run(velb, len, 0, NULL, &code, &err3);
                if (rc != 0 || code != 7) {
                    fprintf(stderr,
                            "vesta_run del IR->velb fallo (rc=%d code=%d): %s\n",
                            rc, code, err3 ? err3 : "(sin mensaje)");
                    vesta_free(err3);
                    fallos++;
                } else {
                    printf("\n--- Caso 1: IR -> velb -> run = %d (esperado 7) OK\n",
                           code);
                }
                vesta_free(velb);
            }
            vesta_free(ir);
        }
    }

    /* --- Caso 2: vesta_json_validate (valido + invalido) --- */
    {
        const char *ok_json = "{\"a\": 1, \"b\": [2, 3], \"c\": null}";
        const char *bad_json = "{\"a\": 1, }"; /* coma colgante => invalido */
        char *err = NULL;
        int rc = vesta_json_validate(ok_json, &err);
        if (rc != 0) {
            fprintf(stderr, "vesta_json_validate del JSON valido fallo: %s\n",
                    err ? err : "(sin mensaje)");
            vesta_free(err);
            fallos++;
        } else {
            printf("\n--- Caso 2a: JSON valido aceptado OK\n");
        }
        err = NULL;
        rc = vesta_json_validate(bad_json, &err);
        if (rc == 0) {
            fprintf(stderr,
                    "vesta_json_validate ACEPTO un JSON invalido (error)\n");
            fallos++;
        } else {
            printf("--- Caso 2b: JSON invalido rechazado OK (err: %s)\n",
                   err ? err : "(sin mensaje)");
            vesta_free(err);
        }
    }

    /* --- Caso 3: vesta_json_format (minify + pretty) --- */
    {
        const char *src = "{ \"x\" :  1 ,  \"y\" : [ 2 , 3 ] }";
        char *mini = NULL, *err = NULL;
        int rc = vesta_json_format(src, -1, &mini, &err); /* compacto */
        if (rc != 0) {
            fprintf(stderr, "vesta_json_format (minify) fallo: %s\n",
                    err ? err : "(sin mensaje)");
            vesta_free(err);
            fallos++;
        } else {
            /* El minificado no debe tener espacios extra. */
            const char *expected = "{\"x\":1,\"y\":[2,3]}";
            if (strcmp(mini, expected) != 0) {
                fprintf(stderr,
                        "minify inesperado: '%s' (esperado '%s')\n", mini,
                        expected);
                fallos++;
            } else {
                printf("\n--- Caso 3a: minify = %s OK\n", mini);
            }
            vesta_free(mini);
        }
        char *pretty = NULL;
        err = NULL;
        rc = vesta_json_format(src, 2, &pretty, &err); /* indent 2 */
        if (rc != 0) {
            fprintf(stderr, "vesta_json_format (pretty) fallo: %s\n",
                    err ? err : "(sin mensaje)");
            vesta_free(err);
            fallos++;
        } else {
            printf("--- Caso 3b: pretty (indent 2):\n%s\n", pretty);
            vesta_free(pretty);
        }
    }

    /* --- Caso 4: vesta_sqlite_exec (BD :memory: -> JSON de filas) --- */
    {
        const char *sql =
            "CREATE TABLE t(id INTEGER, name TEXT);"
            "INSERT INTO t VALUES(1,'a'),(2,'b');"
            "SELECT * FROM t;";
        char *json = NULL, *err = NULL;
        int rc = vesta_sqlite_exec(":memory:", sql, &json, &err);
        if (rc != 0) {
            fprintf(stderr, "vesta_sqlite_exec fallo (rc=%d): %s\n", rc,
                    err ? err : "(sin mensaje)");
            vesta_free(err);
            fallos++;
        } else {
            const char *expected = "[{\"id\":1,\"name\":\"a\"},{\"id\":2,\"name\":\"b\"}]";
            if (strcmp(json, expected) != 0) {
                fprintf(stderr,
                        "sqlite JSON inesperado:\n  '%s'\n  esperado '%s'\n",
                        json, expected);
                fallos++;
            } else {
                printf("\n--- Caso 4: sqlite_exec = %s OK\n", json);
            }
            vesta_free(json);
        }
    }

    if (fallos == 0) {
        printf("\nTODOS LOS CASOS P1 OK\n");
        return 0;
    }
    fprintf(stderr, "\n%d CASO(S) P1 FALLARON\n", fallos);
    return 1;
}
