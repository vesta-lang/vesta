/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

// Test verboso del RawAllocator.
// Cada test imprime cada paso relevante y verifica con ASSERT_MSG.

#include "gc/raw_allocator.h"

#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers de salida
// ---------------------------------------------------------------------------

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define SECTION(name)                                                            \
    printf("\n\033[1;36m══════════════════════════════════════════════\033[" \
           "0m\n");                                                              \
    printf("\033[1;36m  %s\033[0m\n", name);                                     \
    printf("\033[1;36m══════════════════════════════════════════════\033["   \
           "0m\n")

#define ASSERT_MSG(cond, msg)                                                  \
    do {                                                                       \
        g_tests_run++;                                                         \
        if (!(cond)) {                                                         \
            printf("  \033[1;31m[FAIL]\033[0m %s  (linea %d)\n", msg,          \
                   __LINE__);                                                  \
            g_tests_failed++;                                                  \
        } else {                                                               \
            printf("  \033[1;32m[OK]  \033[0m %s\n", msg);                     \
            g_tests_passed++;                                                  \
        }                                                                      \
    } while (0)

static void print_raw_stats(const gc::RawStats &s) {
    printf("\n  \033[1;33m[STATS RawAllocator]\033[0m\n");
    printf("    alloc_count   : %llu llamadas\n",
           (unsigned long long)s.alloc_count);
    printf("    alloc_bytes   : %llu bytes\n",
           (unsigned long long)s.alloc_bytes);
    printf("    free_count    : %llu llamadas\n",
           (unsigned long long)s.free_count);
    printf("    freed_bytes   : %llu bytes\n",
           (unsigned long long)s.freed_bytes);
    printf("    realloc_count : %llu llamadas\n",
           (unsigned long long)s.realloc_count);
    printf("    peak_bytes    : %llu bytes\n\n",
           (unsigned long long)s.peak_bytes);
}

// ---------------------------------------------------------------------------
// Test 1: alloc / free basico
// ---------------------------------------------------------------------------

static void test_basic_alloc_free() {
    SECTION("Test 1: alloc / free basico");

    gc::RawAllocator ra;

    uint64_t p1 = ra.alloc(128);
    printf("  alloc(128)       → ptr=0x%llx\n", (unsigned long long)p1);
    ASSERT_MSG(p1 != 0, "alloc(128) retorna puntero no nulo");

    // El puntero debe ser un host ptr real usable directamente
    uint8_t *raw = reinterpret_cast<uint8_t *>(p1);
    std::memset(raw, 0xFF, 128);
    bool ok = true;
    for (int i = 0; i < 128; ++i)
        ok &= (raw[i] == 0xFF);
    printf("  escritura+lectura 128 bytes: %s\n", ok ? "OK" : "FALLO");
    ASSERT_MSG(ok, "escritura/lectura directa via puntero host");

    printf("  total_allocated  = %zu bytes\n", ra.total_allocated());
    printf("  block_count      = %zu bloques\n", ra.block_count());
    ASSERT_MSG(ra.total_allocated() == 128,
               "total_allocated == 128 tras un alloc");
    ASSERT_MSG(ra.block_count() == 1, "block_count == 1 tras un alloc");

    bool freed = ra.free(p1);
    printf("  free(0x%llx)    → %s\n", (unsigned long long)p1,
           freed ? "OK" : "FALLO");
    ASSERT_MSG(freed, "free() retorna true para ptr valido");
    ASSERT_MSG(ra.total_allocated() == 0, "total_allocated == 0 tras free");
    ASSERT_MSG(ra.block_count() == 0, "block_count == 0 tras free");

    print_raw_stats(ra.stats());
    ASSERT_MSG(ra.stats().alloc_count == 1, "stats: alloc_count == 1");
    ASSERT_MSG(ra.stats().alloc_bytes == 128, "stats: alloc_bytes == 128");
    ASSERT_MSG(ra.stats().free_count == 1, "stats: free_count == 1");
    ASSERT_MSG(ra.stats().freed_bytes == 128, "stats: freed_bytes == 128");
}

// ---------------------------------------------------------------------------
// Test 2: Bloque grande (>4096 bytes) — memoria contigua
// ---------------------------------------------------------------------------

static void test_large_alloc_contiguous() {
    SECTION("Test 2: Bloque grande — contigüidad garantizada para FFI");

    gc::RawAllocator ra;

    // 1 MB contiguo
    constexpr size_t SIZE = 1 * 1024 * 1024;
    uint64_t p = ra.alloc(SIZE);
    printf("  alloc(%zu bytes = 1 MB) → ptr=0x%llx\n", SIZE,
           (unsigned long long)p);
    ASSERT_MSG(p != 0, "alloc(1 MB) retorna puntero no nulo");

    uint8_t *raw = reinterpret_cast<uint8_t *>(p);

    // Escribir un patron secuencial en todo el bloque
    printf("  Escribiendo patron secuencial en 1 MB...\n");
    for (size_t i = 0; i < SIZE; ++i)
        raw[i] = static_cast<uint8_t>(i & 0xFF);

    // Verificar que la lectura es contigua (cada byte en orden)
    printf("  Verificando contigüidad leyendo 1 MB...\n");
    bool contig = true;
    for (size_t i = 0; i < SIZE; ++i) {
        if (raw[i] != static_cast<uint8_t>(i & 0xFF)) {
            printf("  FALLO en byte %zu: esperado 0x%02X, leido 0x%02X\n", i,
                   (unsigned)(i & 0xFF), (unsigned)raw[i]);
            contig = false;
            break;
        }
    }
    printf("  Lectura contigua de 1 MB: %s\n", contig ? "OK" : "FALLO");
    ASSERT_MSG(contig, "bloque de 1 MB es contiguo y completamente accesible");

    // Verificar que podemos usarlo como si fuera un array C normal
    int *arr = reinterpret_cast<int *>(p);
    arr[0] = 0xDEADBEEF;
    arr[1] = 0xCAFEBABE;
    ASSERT_MSG(arr[0] == (int)0xDEADBEEF, "uso como array int[] funciona");
    ASSERT_MSG(arr[1] == (int)0xCAFEBABE, "segundo elemento int[] correcto");

    ra.free(p);
    ASSERT_MSG(ra.total_allocated() == 0, "1 MB liberado correctamente");

    print_raw_stats(ra.stats());
    ASSERT_MSG(ra.stats().alloc_bytes == SIZE, "stats: alloc_bytes == 1 MB");
    ASSERT_MSG(ra.stats().freed_bytes == SIZE, "stats: freed_bytes == 1 MB");
    ASSERT_MSG(ra.stats().peak_bytes == SIZE, "stats: peak_bytes == 1 MB");
}

// ---------------------------------------------------------------------------
// Test 3: realloc creciente — datos preservados
// ---------------------------------------------------------------------------

static void test_realloc_grow() {
    SECTION("Test 3: realloc creciente — datos originales preservados");

    gc::RawAllocator ra;

    // Asignar 256 bytes y llenar con patron
    uint64_t p = ra.alloc(256);
    uint8_t *raw = reinterpret_cast<uint8_t *>(p);
    for (int i = 0; i < 256; ++i)
        raw[i] = static_cast<uint8_t>(i);
    printf("  alloc(256)  → ptr=0x%llx, patron secuencial escrito\n",
           (unsigned long long)p);

    // Crecer a 4096 bytes
    uint64_t p2 = ra.realloc(p, 4096);
    printf("  realloc(256→4096) → nuevo ptr=0x%llx\n", (unsigned long long)p2);
    ASSERT_MSG(p2 != 0, "realloc retorna puntero valido");

    uint8_t *raw2 = reinterpret_cast<uint8_t *>(p2);

    // Los primeros 256 bytes deben tener el patron original
    bool preserved = true;
    for (int i = 0; i < 256; ++i) {
        if (raw2[i] != static_cast<uint8_t>(i)) {
            printf("  FALLO: byte[%d] esperado 0x%02X, leido 0x%02X\n", i,
                   i & 0xFF, raw2[i]);
            preserved = false;
            break;
        }
    }
    printf("  datos originales preservados: %s\n", preserved ? "SI" : "NO");
    ASSERT_MSG(preserved,
               "primeros 256 bytes preservados tras realloc creciente");

    // Los bytes nuevos (256..4095) deben estar a cero
    bool zeroed = true;
    for (int i = 256; i < 4096; ++i)
        zeroed &= (raw2[i] == 0);
    printf("  bytes nuevos (256..4095) zero-inicializados: %s\n",
           zeroed ? "SI" : "NO");
    ASSERT_MSG(zeroed, "extension zero-inicializada tras realloc creciente");

    ASSERT_MSG(ra.total_allocated() == 4096,
               "total_allocated == 4096 tras realloc");

    ra.free(p2);
    print_raw_stats(ra.stats());
    ASSERT_MSG(ra.stats().realloc_count == 1, "stats: realloc_count == 1");
}

// ---------------------------------------------------------------------------
// Test 4: realloc decreciente — datos del area comun preservados
// ---------------------------------------------------------------------------

static void test_realloc_shrink() {
    SECTION("Test 4: realloc decreciente — datos del area comun preservados");

    gc::RawAllocator ra;

    uint64_t p = ra.alloc(1024);
    uint8_t *raw = reinterpret_cast<uint8_t *>(p);
    for (int i = 0; i < 1024; ++i)
        raw[i] = static_cast<uint8_t>(i & 0xFF);
    printf("  alloc(1024) → patron secuencial escrito\n");

    uint64_t p2 = ra.realloc(p, 64);
    printf("  realloc(1024→64) → nuevo ptr=0x%llx\n", (unsigned long long)p2);
    ASSERT_MSG(p2 != 0, "realloc decreciente retorna puntero valido");

    uint8_t *raw2 = reinterpret_cast<uint8_t *>(p2);
    bool preserved = true;
    for (int i = 0; i < 64; ++i) {
        if (raw2[i] != static_cast<uint8_t>(i & 0xFF)) {
            preserved = false;
            break;
        }
    }
    printf("  primeros 64 bytes preservados: %s\n", preserved ? "SI" : "NO");
    ASSERT_MSG(preserved, "area comun preservada tras realloc decreciente");
    ASSERT_MSG(ra.total_allocated() == 64, "total_allocated == 64 tras shrink");

    ra.free(p2);
    print_raw_stats(ra.stats());
}

// ---------------------------------------------------------------------------
// Test 5: free de puntero desconocido retorna false
// ---------------------------------------------------------------------------

static void test_free_unknown() {
    SECTION("Test 5: free de puntero desconocido — retorna false sin crash");

    gc::RawAllocator ra;

    // free de ptr que nunca se asigno
    bool r1 = ra.free(0xDEADBEEFDEADBEEFULL);
    printf("  free(0xDEADBEEFDEADBEEF) → %s\n", r1 ? "true" : "false");
    ASSERT_MSG(!r1, "free de ptr desconocido retorna false");

    // free de 0
    bool r2 = ra.free(0);
    printf("  free(0)                  → %s\n", r2 ? "true" : "false");
    ASSERT_MSG(!r2, "free(0) retorna false");

    // Asignar, liberar, volver a liberar el mismo ptr (double-free seguro)
    uint64_t p = ra.alloc(64);
    ra.free(p);
    bool r3 = ra.free(p); // segunda liberacion: ya no existe en el mapa
    printf("  double-free(0x%llx)     → %s\n", (unsigned long long)p,
           r3 ? "true" : "false");
    ASSERT_MSG(!r3, "double-free retorna false sin crash");

    ASSERT_MSG(ra.stats().alloc_count == 1,
               "stats: 1 alloc total en este test");
    ASSERT_MSG(ra.stats().free_count == 1, "stats: solo 1 free exitoso");
    print_raw_stats(ra.stats());
}

// ---------------------------------------------------------------------------
// Test 6: multiples allocs y free_all
// ---------------------------------------------------------------------------

static void test_free_all() {
    SECTION("Test 6: Multiples allocs y free_all");

    gc::RawAllocator ra;

    constexpr int N = 10;
    constexpr size_t SIZES[N] = {64,   128,  256,  512,   1024,
                                 2048, 4096, 8192, 16384, 32768};
    size_t total_expected = 0;

    printf("  Asignando %d bloques de distintos tamaños...\n", N);
    for (int i = 0; i < N; ++i) {
        uint64_t p = ra.alloc(SIZES[i]);
        total_expected += SIZES[i];
        printf("    alloc(%5zu) → ptr=0x%llx\n", SIZES[i],
               (unsigned long long)p);
        ASSERT_MSG(p != 0, "alloc retorna puntero valido");
    }

    printf("  total_allocated = %zu (esperado %zu)\n", ra.total_allocated(),
           total_expected);
    ASSERT_MSG(ra.total_allocated() == total_expected,
               "total_allocated correcto");
    ASSERT_MSG(ra.block_count() == N, "block_count == N");

    printf("  >>> free_all()...\n");
    ra.free_all();

    printf("  total_allocated = %zu (esperado 0)\n", ra.total_allocated());
    ASSERT_MSG(ra.total_allocated() == 0, "total_allocated == 0 tras free_all");
    ASSERT_MSG(ra.block_count() == 0, "block_count == 0 tras free_all");

    print_raw_stats(ra.stats());
    ASSERT_MSG(ra.stats().alloc_count == N, "stats: alloc_count == N");
    ASSERT_MSG(ra.stats().free_count == N,
               "stats: free_count == N tras free_all");
    ASSERT_MSG(ra.stats().freed_bytes == total_expected,
               "stats: freed_bytes == total asignado");
    ASSERT_MSG(ra.stats().peak_bytes == total_expected,
               "stats: peak_bytes == maximo simultaneo");
}

// ---------------------------------------------------------------------------
// Test 7: realloc(ptr, 0) equivale a free
// ---------------------------------------------------------------------------

static void test_realloc_to_zero() {
    SECTION("Test 7: realloc(ptr, 0) equivale a free");

    gc::RawAllocator ra;

    uint64_t p = ra.alloc(512);
    printf("  alloc(512) → ptr=0x%llx\n", (unsigned long long)p);
    ASSERT_MSG(p != 0, "alloc(512) valido");

    uint64_t r = ra.realloc(p, 0);
    printf("  realloc(ptr, 0) → 0x%llx\n", (unsigned long long)r);
    ASSERT_MSG(r == 0, "realloc(ptr,0) retorna 0");
    ASSERT_MSG(ra.total_allocated() == 0,
               "total_allocated == 0 tras realloc-free");
    ASSERT_MSG(ra.block_count() == 0, "block_count == 0 tras realloc-free");

    print_raw_stats(ra.stats());
    ASSERT_MSG(ra.stats().realloc_count == 1, "stats: realloc_count == 1");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
#if defined(WIN32) || defined(_WIN32) ||                                       \
    defined(__WIN32) && !defined(__CYGWIN__)
    system("chcp 65001");
#endif
    printf("\n\033[1;35m╔══════════════════════════════════════════════╗\033["
           "0m\n");
    printf(
        "\033[1;35m║       TEST SUITE: RawAllocator               ║\033[0m\n");
    printf(
        "\033[1;35m╚══════════════════════════════════════════════╝\033[0m\n");

    test_basic_alloc_free();
    test_large_alloc_contiguous();
    test_realloc_grow();
    test_realloc_shrink();
    test_free_unknown();
    test_free_all();
    test_realloc_to_zero();

    printf("\n\033[1;35m╔══════════════════════════════════════════════╗\033["
           "0m\n");
    printf(
        "\033[1;35m║  RESULTADO FINAL                             ║\033[0m\n");
    printf(
        "\033[1;35m╠══════════════════════════════════════════════╣\033[0m\n");
    printf(
        "\033[1;35m║  Tests ejecutados : %-4d                     ║\033[0m\n",
        g_tests_run);
    printf(
        "\033[1;32m║  Pasados          : %-4d                     ║\033[0m\n",
        g_tests_passed);
    if (g_tests_failed > 0)
        printf("\033[1;31m║  Fallados         : %-4d                     "
               "║\033[0m\n",
               g_tests_failed);
    else
        printf("\033[1;32m║  Fallados         : 0                        "
               "║\033[0m\n");
    printf("\033[1;35m╚══════════════════════════════════════════════╝\033["
           "0m\n\n");

    return g_tests_failed == 0 ? 0 : 1;
}
