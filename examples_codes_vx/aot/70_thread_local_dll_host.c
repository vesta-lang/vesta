/* Host C de prueba para 70_thread_local_dll.vx.
 *
 * Carga la .dll AOT con LoadLibrary, obtiene get_t/reset_t con GetProcAddress,
 * y verifica DOS cosas:
 *   1) La plantilla TLS se aplica: la primera get_t() del hilo principal da 12
 *      (7 + 5), probando que el AddressOfEntryPoint -> __vx_tls_init aplico el
 *      valor inicial 7.
 *   2) Aislamiento por-hilo: un hilo nuevo (CreateThread) empieza su copia en 7
 *      (no comparte con el principal), asi su get_t() tambien da 12.
 *
 * Resultado esperado: exit 0 (todas las aserciones OK).  Imprime el detalle.
 *
 * Compilar (MinGW):  gcc 70_thread_local_dll_host.c -o host.exe
 * Ejecutar:          host.exe tls.dll
 */
#include <windows.h>
#include <stdio.h>

typedef long long (*fn0)(void);

static fn0 g_get_t;
static long long g_child_first;

static DWORD WINAPI worker(LPVOID p) {
    (void)p;
    /* Hilo nuevo: su copia TLS de `t` debe empezar en 7 (aislada). */
    g_child_first = g_get_t();   /* esperado 12 (7 + 5) */
    return 0;
}

int main(int argc, char **argv) {
    const char *dll = (argc > 1) ? argv[1] : "tls.dll";
    HMODULE h = LoadLibraryA(dll);
    if (!h) { printf("LoadLibrary('%s') fallo: %lu\n", dll, GetLastError()); return 2; }

    g_get_t = (fn0)GetProcAddress(h, "get_t");
    if (!g_get_t) { printf("get_t no encontrada\n"); return 2; }

    long long main_first = g_get_t();        /* 12: plantilla 7 + 5 */
    long long main_second = g_get_t();        /* 17: misma copia + 5 */

    HANDLE th = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    WaitForSingleObject(th, INFINITE);

    printf("main_first=%lld (esp 12)  main_second=%lld (esp 17)  child_first=%lld (esp 12)\n",
           main_first, main_second, g_child_first);

    int ok = (main_first == 12) && (main_second == 17) && (g_child_first == 12);
    printf(ok ? "OK: plantilla TLS + aislamiento por-hilo correctos\n"
              : "FALLO\n");
    return ok ? 0 : 1;
}
