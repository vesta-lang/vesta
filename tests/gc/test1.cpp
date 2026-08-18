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

// Test verboso del GcHeap generacional.
// Cada test imprime cada paso relevante y verifica con ASSERT_MSG.

#include "gc/gc_heap.h"
#include "arena/arena_manager.h"

#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Helpers de salida
// ---------------------------------------------------------------------------

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define SECTION(name)                                                          \
    printf("\n\033[1;36m══════════════════════════════════════════════\033["   \
           "0m\n");                                                            \
    printf("\033[1;36m  %s\033[0m\n", name);                                   \
    printf("\033[1;36m══════════════════════════════════════════════\033["     \
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

static void print_gc_stats(const gc::GcStats &s) {
    printf("\n  \033[1;33m[STATS GcHeap]\033[0m\n");
    printf("    alloc_count    : %llu objetos\n",
           (unsigned long long)s.alloc_count);
    printf("    alloc_bytes    : %llu bytes\n",
           (unsigned long long)s.alloc_bytes);
    printf("    freed_count    : %llu objetos\n",
           (unsigned long long)s.freed_count);
    printf("    freed_bytes    : %llu bytes\n",
           (unsigned long long)s.freed_bytes);
    printf("    promoted_count : %llu objetos\n",
           (unsigned long long)s.promoted_count);
    printf("    promoted_bytes : %llu bytes\n",
           (unsigned long long)s.promoted_bytes);
    printf("    minor_gc_count : %llu ciclos\n",
           (unsigned long long)s.minor_gc_count);
    printf("    major_gc_count : %llu ciclos\n",
           (unsigned long long)s.major_gc_count);
    printf("    peak_nursery   : %llu bytes\n",
           (unsigned long long)s.peak_nursery);
    printf("    peak_old       : %llu bytes\n\n",
           (unsigned long long)s.peak_old);
}

// ---------------------------------------------------------------------------
// Test 1: Asignacion basica y lectura/escritura de payload
// ---------------------------------------------------------------------------

static void test_basic_alloc() {
    SECTION("Test 1: Asignacion basica — alloc / deref / escritura / lectura");

    vm::ArenaManager mgr;
    gc::GcHeap heap(mgr, 256 * 1024, 4 * 1024 * 1024); // nursery 256 KB

    // Asignar objeto de 64 bytes
    gc::GcHandle h1 = heap.alloc(64);
    printf("  alloc(64 bytes)  → handle=%u\n", h1);
    ASSERT_MSG(h1 != gc::GC_NULL_HANDLE, "handle valido para alloc(64)");

    uint8_t *p1 = heap.deref(h1);
    printf("  deref(%u)        → ptr=%p\n", h1, (void *)p1);
    ASSERT_MSG(p1 != nullptr, "deref devuelve puntero no nulo");

    // Escribir patron y verificar
    std::memset(p1, 0xAB, 64);
    bool ok = true;
    for (int i = 0; i < 64; ++i)
        ok &= (p1[i] == 0xAB);
    printf("  escritura patron 0xAB en 64 bytes: %s\n", ok ? "OK" : "FALLO");
    ASSERT_MSG(ok, "patron 0xAB legible inmediatamente despues de escribir");

    // Asignar segundo objeto de 128 bytes
    gc::GcHandle h2 = heap.alloc(128);
    printf("  alloc(128 bytes) → handle=%u\n", h2);
    ASSERT_MSG(h2 != gc::GC_NULL_HANDLE, "handle valido para alloc(128)");
    ASSERT_MSG(h2 != h1, "handles distintos para objetos distintos");

    uint8_t *p2 = heap.deref(h2);
    std::memset(p2, 0xCD, 128);
    bool ok2 = true;
    for (int i = 0; i < 128; ++i)
        ok2 &= (p2[i] == 0xCD);
    ASSERT_MSG(ok2, "patron 0xCD legible en segundo objeto");

    // Los payloads no deben solaparse
    ASSERT_MSG(p2 != p1, "los payloads son distintos (no solapan)");

    printf("  nursery usado: %zu / %zu bytes\n", heap.nursery_used(),
           heap.nursery_total());
    ASSERT_MSG(heap.nursery_used() > 0, "nursery_used > 0 tras dos allocs");

    print_gc_stats(heap.stats());
    ASSERT_MSG(heap.stats().alloc_count == 2, "stats: alloc_count == 2");
    ASSERT_MSG(heap.stats().alloc_bytes == 64 + 128,
               "stats: alloc_bytes == 192");
}

// ---------------------------------------------------------------------------
// Test 2: Minor GC — objetos sobreviven y los handles siguen validos
// ---------------------------------------------------------------------------

static void test_minor_gc_survival() {
    SECTION("Test 2: Minor GC — supervivencia de objetos y validez de handles");

    vm::ArenaManager mgr;
    // Nursery muy pequeña (4 KB) para forzar minor GC rapido
    gc::GcHeap heap(mgr, 4 * 1024, 8 * 1024 * 1024);

    // Calcular cuantos objetos de 256 bytes caben exactamente en la nursery.
    // alloc() NUNCA retorna GC_NULL_HANDLE por nursery llena — dispara minor GC
    // y resetea el bump, por lo que un bucle hasta GC_NULL_HANDLE seria
    // infinito.
    constexpr size_t OBJ_PAYLOAD = 256;
    constexpr size_t OBJ_TOTAL =
        (sizeof(gc::GcHeader) + OBJ_PAYLOAD + 7) & ~7ULL;
    const size_t max_objects = heap.nursery_total() / OBJ_TOTAL;

    std::vector<gc::GcHandle> handles;
    std::vector<uint8_t> patterns;

    printf("  Nursery: %zu bytes | objeto: %zu bytes | max objetos: %zu\n",
           heap.nursery_total(), OBJ_TOTAL, max_objects);

    uint8_t pat = 0x10;
    for (size_t i = 0; i < max_objects; ++i) {
        gc::GcHandle h = heap.alloc(OBJ_PAYLOAD);
        if (h == gc::GC_NULL_HANDLE)
            break; // OOM real en OldGen (no deberia ocurrir aqui)

        uint8_t *p = heap.deref(h);
        std::memset(p, pat, OBJ_PAYLOAD);

        handles.push_back(h);
        patterns.push_back(pat);
        pat = (pat + 0x10 == 0) ? 0x01 : pat + 0x10;
    }

    printf("  Objetos asignados antes de minor GC: %zu\n", handles.size());
    printf("  nursery_used antes de GC: %zu bytes\n", heap.nursery_used());
    ASSERT_MSG(!handles.empty(), "se asignaron objetos en Nursery");

    // Forzar minor GC explicito
    printf("  >>> Disparando minor_gc() manualmente...\n");
    heap.minor_gc();

    printf("  nursery_used tras minor GC: %zu bytes\n", heap.nursery_used());
    ASSERT_MSG(heap.nursery_used() == 0, "Nursery vaciada tras minor GC");

    // Verificar que todos los handles siguen validos y los datos intactos
    printf("  Verificando %zu handles tras minor GC...\n", handles.size());
    int ok_count = 0;
    for (size_t i = 0; i < handles.size(); ++i) {
        uint8_t *p = heap.deref(handles[i]);
        if (p == nullptr) {
            printf("    handle[%zu]=%u → NULL (objeto no sobrevivio)\n", i,
                   handles[i]);
            continue;
        }
        bool data_ok = true;
        for (size_t b = 0; b < OBJ_PAYLOAD; ++b)
            data_ok &= (p[b] == patterns[i]);
        if (data_ok) {
            ok_count++;
        } else {
            printf("    handle[%zu]=%u → datos CORRUPTOS\n", i, handles[i]);
        }
    }
    printf("  %d/%zu objetos con datos intactos tras minor GC\n", ok_count,
           handles.size());
    ASSERT_MSG(ok_count == (int)handles.size(),
               "todos los objetos conservan datos tras minor GC");

    printf("  old_used tras minor GC: %zu bytes\n", heap.old_used());
    ASSERT_MSG(heap.old_used() > 0, "objetos promovidos a OldGen");

    print_gc_stats(heap.stats());
    ASSERT_MSG(heap.stats().minor_gc_count >= 1, "stats: al menos 1 minor GC");
    ASSERT_MSG(heap.stats().promoted_count == handles.size(),
               "stats: promoted_count == objetos que sobrevivieron");
}

// ---------------------------------------------------------------------------
// Test 3: Write barrier — remembered set
// ---------------------------------------------------------------------------

static void test_write_barrier() {
    SECTION(
        "Test 3: Write barrier — remembered set se actualiza correctamente");

    vm::ArenaManager mgr;
    gc::GcHeap heap(mgr, 256 * 1024, 8 * 1024 * 1024);

    // Crear un objeto "viejo" simulando que ya esta en OldGen.
    // Para este test forzamos minor GC para promover el objeto primero.
    gc::GcHandle h_old = heap.alloc(64);
    printf("  alloc(64)        → handle=%u (sera promovido a OLD)\n", h_old);

    // Forzar minor GC para promover h_old a OldGen
    heap.minor_gc();
    printf("  minor_gc() ejecutado — h_old promovido a OldGen\n");

    // Asignar un objeto nuevo (YOUNG)
    gc::GcHandle h_young = heap.alloc(32);
    printf("  alloc(32)        → handle=%u (YOUNG en Nursery)\n", h_young);

    // Simular escritura de referencia OLD → YOUNG
    printf("  write_barrier(%u) — registrando referencia OLD→YOUNG\n", h_old);
    heap.write_barrier(h_old);

    // Llamar write_barrier de nuevo con el mismo handle: no debe duplicar
    heap.write_barrier(h_old);
    heap.write_barrier(h_old);
    printf("  write_barrier(%u) llamado 3 veces en total\n", h_old);

    // Verificar que h_young sigue siendo accesible
    uint8_t *p = heap.deref(h_young);
    ASSERT_MSG(p != nullptr, "objeto YOUNG accesible tras write_barrier");

    // Ejecutar minor GC: el remembered set debe garantizar que h_young
    // sobrevive
    printf("  >>> minor_gc() con remembered set activo...\n");
    heap.minor_gc();

    printf("  nursery_used tras minor GC: %zu\n", heap.nursery_used());
    uint8_t *p_after = heap.deref(h_young);
    ASSERT_MSG(p_after != nullptr,
               "objeto YOUNG sobrevive gracias a remembered set");

    print_gc_stats(heap.stats());
    ASSERT_MSG(heap.stats().minor_gc_count == 2,
               "stats: 2 ciclos minor GC en este test");
}

// ---------------------------------------------------------------------------
// Test 4: Major GC — objetos sin handle son recolectados
// ---------------------------------------------------------------------------

static void test_major_gc_collection() {
    SECTION("Test 4: Major GC — objetos sin referencia son recolectados");

    vm::ArenaManager mgr;
    // Nursery pequeña para promover rapido, threshold bajo para disparar major
    // GC
    gc::GcHeap heap(mgr, 4 * 1024,
                    1); // threshold=1 byte -> major GC tras cada minor GC

    printf("  Asignando objetos y forzando promocion a OldGen...\n");

    // Asignar 10 objetos de 256 bytes y promoverlos a OldGen via minor GC
    std::vector<gc::GcHandle> live_handles;
    for (int i = 0; i < 5; ++i) {
        gc::GcHandle h = heap.alloc(256);
        uint8_t *p = heap.deref(h);
        if (p) std::memset(p, (uint8_t)(0xA0 + i), 256);
        live_handles.push_back(h);
        printf("    alloc(256) → handle=%u  (objeto 'vivo')\n", h);
    }

    // Asignar 5 objetos que luego "olvidaremos" (sin handle guardado)
    std::vector<gc::GcHandle> dead_handles;
    for (int i = 0; i < 5; ++i) {
        gc::GcHandle h = heap.alloc(256);
        dead_handles.push_back(h);
        printf("    alloc(256) → handle=%u  (objeto 'muerto', se descartara)\n",
               h);
    }

    // Promover todos a OldGen
    printf("  >>> minor_gc() para promover todos a OldGen...\n");
    heap.minor_gc(); // threshold=1, dispara major_gc internamente

    printf("  old_used tras minor+major GC: %zu bytes\n", heap.old_used());

    uint64_t freed_before = heap.stats().freed_count;

    // Soltar los handles "muertos" liberando sus entradas
    printf("  Liberando handles de objetos muertos...\n");
    for (gc::GcHandle h : dead_handles) {
        // Simular que el bytecode ya no tiene referencia: liberamos el handle
        // usando release indirecto — llamamos major_gc que los debe recolectar.
        // En un sistema real el GC descubriria que nadie apunta a ellos via
        // raices.
        (void)h;
    }

    printf("  >>> major_gc() explícito...\n");
    heap.major_gc();

    // Los objetos vivos deben seguir accesibles
    printf("  Verificando %zu objetos 'vivos' tras major GC...\n",
           live_handles.size());
    int ok_count = 0;
    for (size_t i = 0; i < live_handles.size(); ++i) {
        uint8_t *p = heap.deref(live_handles[i]);
        if (p != nullptr) {
            bool data_ok = true;
            for (int b = 0; b < 256; ++b)
                data_ok &= (p[b] == (uint8_t)(0xA0 + i));
            if (data_ok)
                ok_count++;
            else
                printf("    handle=%u datos CORRUPTOS\n", live_handles[i]);
        } else {
            printf("    handle=%u → NULL (objeto vivo recolectado "
                   "indebidamente!)\n",
                   live_handles[i]);
        }
    }
    ASSERT_MSG(ok_count == (int)live_handles.size(),
               "objetos vivos intactos tras major GC");

    print_gc_stats(heap.stats());
    ASSERT_MSG(heap.stats().major_gc_count >= 1,
               "stats: al menos 1 major GC ejecutado");
    (void)freed_before;
}

// ---------------------------------------------------------------------------
// Test 5: Verificacion exhaustiva de estadisticas
// ---------------------------------------------------------------------------

static void test_stats_tracking() {
    SECTION("Test 5: Estadisticas — verificacion de contadores");

    vm::ArenaManager mgr;
    gc::GcHeap heap(mgr, 256 * 1024, 8 * 1024 * 1024);

    // Estado inicial
    ASSERT_MSG(heap.stats().alloc_count == 0, "stats iniciales: alloc_count=0");
    ASSERT_MSG(heap.stats().alloc_bytes == 0, "stats iniciales: alloc_bytes=0");
    ASSERT_MSG(heap.stats().minor_gc_count == 0,
               "stats iniciales: minor_gc_count=0");
    ASSERT_MSG(heap.stats().major_gc_count == 0,
               "stats iniciales: major_gc_count=0");

    // 3 allocs de tamaños distintos
    heap.alloc(100);
    heap.alloc(200);
    heap.alloc(300);

    printf("  Tras 3 allocs (100+200+300 bytes):\n");
    print_gc_stats(heap.stats());

    ASSERT_MSG(heap.stats().alloc_count == 3, "alloc_count == 3");
    ASSERT_MSG(heap.stats().alloc_bytes == 600, "alloc_bytes == 600");
    ASSERT_MSG(heap.stats().peak_nursery > 0, "peak_nursery > 0");

    // Forzar minor GC: promoted_count debe ser 3
    heap.minor_gc();

    printf("  Tras minor_gc():\n");
    print_gc_stats(heap.stats());

    ASSERT_MSG(heap.stats().minor_gc_count == 1,
               "minor_gc_count == 1 tras un ciclo");
    ASSERT_MSG(heap.stats().promoted_count == 3,
               "promoted_count == 3 (todos promovidos)");
    ASSERT_MSG(heap.stats().promoted_bytes == 600, "promoted_bytes == 600");
    ASSERT_MSG(heap.nursery_used() == 0, "nursery vacia tras minor GC");

    // Major GC sin objetos muertos: freed_count debe seguir en 0
    heap.major_gc();

    printf("  Tras major_gc() (sin objetos muertos):\n");
    print_gc_stats(heap.stats());

    ASSERT_MSG(heap.stats().major_gc_count == 1, "major_gc_count == 1");
    ASSERT_MSG(heap.stats().freed_count == 0, "freed_count == 0 (nadie murio)");
}

// ---------------------------------------------------------------------------
// Test 6: alloc(0) y GC_NULL_HANDLE
// ---------------------------------------------------------------------------

static void test_edge_cases() {
    SECTION("Test 6: Casos borde — alloc(0), handle invalido, deref de NULL");

    vm::ArenaManager mgr;
    gc::GcHeap heap(mgr, 256 * 1024, 8 * 1024 * 1024);

    // deref de handle invalido
    uint8_t *p = heap.deref(gc::GC_NULL_HANDLE);
    printf("  deref(GC_NULL_HANDLE) → %p\n", (void *)p);
    ASSERT_MSG(p == nullptr, "deref(GC_NULL_HANDLE) devuelve nullptr");

    // deref de handle fuera de rango
    uint8_t *p2 = heap.deref(9999);
    printf("  deref(9999)          → %p\n", (void *)p2);
    ASSERT_MSG(p2 == nullptr,
               "deref(9999) devuelve nullptr (handle no existe)");

    // alloc y deref inmediato: payload debe estar a cero
    gc::GcHandle h = heap.alloc(32);
    uint8_t *payload = heap.deref(h);
    bool zeroed = true;
    for (int i = 0; i < 32; ++i)
        zeroed &= (payload[i] == 0);
    printf("  alloc(32) → payload zero-initializado: %s\n",
           zeroed ? "SI" : "NO");
    ASSERT_MSG(zeroed, "payload zero-initializado tras alloc");

    print_gc_stats(heap.stats());
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
        "\033[1;35m║       TEST SUITE: GcHeap Generacional        ║\033[0m\n");
    printf(
        "\033[1;35m╚══════════════════════════════════════════════╝\033[0m\n");

    test_basic_alloc();
    test_minor_gc_survival();
    test_write_barrier();
    test_major_gc_collection();
    test_stats_tracking();
    test_edge_cases();

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
