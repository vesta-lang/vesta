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

/**
 * Test 3 — Simulacion de sistema de particulas con GcHeap generacional.
 *
 * El escenario demuestra la HIPOTESIS GENERACIONAL en accion:
 * "la mayoria de los objetos mueren jovenes".
 *
 * Cada "particula" es un objeto GC con posicion, velocidad, vida restante e ID.
 * El simulador crea N particulas por tick con vidas aleatorias cortas o largas.
 * Cuando una particula muere, su handle se suelta (drop) y queda como basura.
 *
 * Se observa:
 *   - Minor GC recoge la mayoria de particulas antes de que abandonen la
 * Nursery.
 *   - Las particulas de larga vida se promocionan a OldGen.
 *   - Major GC recoge las particulas promovidas que finalmente mueren.
 *   - Las estadisticas muestran la proporcion joven/viejo de la basura.
 */

#include "gc/gc_heap.h"
#include "arena/arena_manager.h"

#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>

// ---------------------------------------------------------------------------
// PRNG determinista (LCG) — sin rand() para reproducibilidad
// ---------------------------------------------------------------------------

static uint64_t g_rng_state = 0xDEADBEEFCAFEBABEULL;

static uint32_t rng_next() {
    g_rng_state = g_rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<uint32_t>(g_rng_state >> 33);
}

static uint32_t rng_range(uint32_t lo, uint32_t hi) {
    return lo + rng_next() % (hi - lo + 1);
}

// ---------------------------------------------------------------------------
// Layout del objeto "Particula" dentro del payload GC
// ---------------------------------------------------------------------------

struct Particle {
    int32_t x, y;     // posicion actual
    int32_t vx, vy;   // velocidad (pixeles por tick)
    int32_t lifetime; // ticks restantes de vida
    uint32_t id;      // ID unico para trazabilidad
};
static_assert(sizeof(Particle) == 24, "Particle debe medir 24 bytes");

static Particle *pget(gc::GcHeap &heap, gc::GcHandle h) {
    return reinterpret_cast<Particle *>(heap.deref(h));
}

// ---------------------------------------------------------------------------
// Helpers de salida
// ---------------------------------------------------------------------------

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

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

static void print_stats(const gc::GcStats &s, size_t nursery_used,
                        size_t nursery_total, size_t old_used) {
    printf("  ┌─────────────────────────────────────────────┐\n");
    printf("  │ STATS GcHeap                                │\n");
    printf("  ├─────────────────────────────────────────────┤\n");
    printf("  │  Nursery:  %6zu / %-6zu bytes             │\n", nursery_used,
           nursery_total);
    printf("  │  OldGen:   %6zu bytes en uso               │\n", old_used);
    printf("  ├─────────────────────────────────────────────┤\n");
    printf("  │  alloc_count    : %-8llu objetos          │\n",
           (unsigned long long)s.alloc_count);
    printf("  │  alloc_bytes    : %-8llu bytes            │\n",
           (unsigned long long)s.alloc_bytes);
    printf("  │  promoted_count : %-8llu objetos          │\n",
           (unsigned long long)s.promoted_count);
    printf("  │  promoted_bytes : %-8llu bytes            │\n",
           (unsigned long long)s.promoted_bytes);
    printf("  │  freed_count    : %-8llu objetos (major)  │\n",
           (unsigned long long)s.freed_count);
    printf("  │  freed_bytes    : %-8llu bytes   (major)  │\n",
           (unsigned long long)s.freed_bytes);
    printf("  │  minor_gc_count : %-8llu ciclos           │\n",
           (unsigned long long)s.minor_gc_count);
    printf("  │  major_gc_count : %-8llu ciclos           │\n",
           (unsigned long long)s.major_gc_count);
    printf("  │  peak_nursery   : %-8llu bytes            │\n",
           (unsigned long long)s.peak_nursery);
    printf("  │  peak_old       : %-8llu bytes            │\n",
           (unsigned long long)s.peak_old);
    printf("  └─────────────────────────────────────────────┘\n");
}

static void bar(const char *label, uint64_t value, uint64_t total,
                int width = 30) {
    int filled = total > 0 ? (int)(value * width / total) : 0;
    printf("  %-18s [", label);
    for (int i = 0; i < width; ++i)
        putchar(i < filled ? '#' : '.');
    printf("] %llu/%llu\n", (unsigned long long)value,
           (unsigned long long)total);
}

// ---------------------------------------------------------------------------
// Simulacion principal
// ---------------------------------------------------------------------------

static void run_particle_simulation() {
    printf(
        "\n\033[1;36m══════════════════════════════════════════════\033[0m\n");
    printf(
        "\033[1;36m  Simulacion de particulas — GC Generacional   \033[0m\n");
    printf(
        "\033[1;36m══════════════════════════════════════════════\033[0m\n\n");

    // Nursery pequeña para provocar GC frecuente y visible.
    // Con objetos de 24+8=32 bytes, la nursery de 2 KB aguanta 64 particulas.
    // El old_threshold bajo (8 KB) provoca major GC tras pocos ciclos.
    constexpr size_t NURSERY_SIZE = 2 * 1024;
    constexpr size_t OLD_THRESHOLD = 8 * 1024;
    constexpr int TICKS = 120;
    constexpr int SPAWN_PER_TICK = 5;

    vm::ArenaManager mgr;
    gc::GcHeap heap(mgr, NURSERY_SIZE, OLD_THRESHOLD);

    // Particulas activas: el simulador mantiene esta lista como "raices"
    std::vector<gc::GcHandle> alive;
    alive.reserve(256);

    uint32_t next_id = 0;
    int tick = 0;
    uint64_t born_total = 0;
    uint64_t died_young =
        0;                 // murieron sin salir de Nursery (minor GC las borro)
    uint64_t died_old = 0; // murieron en OldGen (major GC las borro)

    // snapshot de minor/major GC antes del tick para detectar ciclos nuevos
    uint64_t last_minor = 0;
    uint64_t last_major = 0;

    printf("  Configuracion:\n");
    printf("    Nursery        = %zu bytes\n", NURSERY_SIZE);
    printf("    OldGen umbral  = %zu bytes\n", OLD_THRESHOLD);
    printf("    Ticks          = %d\n", TICKS);
    printf("    Spawn/tick     = %d\n", SPAWN_PER_TICK);
    printf("    Lifetime corta = 1-4 ticks  (80%% de las particulas)\n");
    printf("    Lifetime larga = 10-25 ticks (20%% de las particulas)\n");
    printf("\n");

    for (tick = 1; tick <= TICKS; ++tick) {
        // --- SPAWN: crear nuevas particulas ---
        for (int s = 0; s < SPAWN_PER_TICK; ++s) {
            // 80% vida corta (mueren en nursery), 20% vida larga (se promueven)
            int32_t life = (rng_next() % 10 < 8) ? (int32_t)rng_range(1, 4)
                                                 : (int32_t)rng_range(10, 25);

            gc::GcHandle h = heap.alloc(sizeof(Particle));
            if (h == gc::GC_NULL_HANDLE) continue;

            Particle *p = pget(heap, h);
            p->x = (int32_t)rng_range(0, 1920);
            p->y = (int32_t)rng_range(0, 1080);
            p->vx = (int32_t)rng_range(0, 10) - 5;
            p->vy = (int32_t)rng_range(0, 10) - 5;
            p->lifetime = life;
            p->id = next_id++;

            alive.push_back(h);
            born_total++;
        }

        // --- UPDATE: mover particulas y reducir vida ---
        std::vector<gc::GcHandle> still_alive;
        still_alive.reserve(alive.size());

        for (gc::GcHandle h : alive) {
            Particle *p = pget(heap, h);
            if (!p) {
                // El GC movio el objeto (evacuacion): deref sigue valido
                // porque el handle apunta a la nueva ubicacion.
                // Si deref es null, el handle fue liberado — no deberia pasar
                // aqui.
                continue;
            }

            p->x += p->vx;
            p->y += p->vy;
            p->lifetime--;

            if (p->lifetime <= 0) {
                // Particula muerta: soltar el handle
                heap.drop(h);
            } else {
                still_alive.push_back(h);
            }
        }
        alive = std::move(still_alive);

        // --- DIAGNOSTICO por tick ---
        const gc::GcStats &st = heap.stats();
        bool minor_fired = (st.minor_gc_count > last_minor);
        bool major_fired = (st.major_gc_count > last_major);

        if (minor_fired || major_fired || tick % 10 == 0 || tick == 1) {
            printf("  Tick %3d │ vivas=%3zu │ nursery=%4zu/%4zu │ old=%5zu",
                   tick, alive.size(), heap.nursery_used(),
                   heap.nursery_total(), heap.old_used());

            if (minor_fired)
                printf(" │ \033[1;33m[MINOR GC #%llu]\033[0m",
                       (unsigned long long)st.minor_gc_count);
            if (major_fired)
                printf(" │ \033[1;31m[MAJOR GC #%llu]\033[0m",
                       (unsigned long long)st.major_gc_count);

            printf("\n");
        }

        // calcular particulas muertas por tipo para las estadisticas finales
        // (aproximacion: promovidas = promovidas segun stats; el resto murio
        // joven)
        last_minor = st.minor_gc_count;
        last_major = st.major_gc_count;
    }

    // Tick final: soltar todas las particulas restantes para que major GC las
    // limpie
    printf("\n  Soltando %zu particulas supervivientes...\n", alive.size());
    for (gc::GcHandle h : alive)
        heap.drop(h);
    alive.clear();

    printf("  >>> major_gc() final para limpiar OldGen...\n");
    heap.major_gc();

    // ---------------------------------------------------------------------------
    // Resumen y validaciones
    // ---------------------------------------------------------------------------

    const gc::GcStats &st = heap.stats();

    printf(
        "\n  "
        "\033[1;35m═══════════════════════════════════════════════\033[0m\n");
    printf("  \033[1;35m  RESUMEN DE LA SIMULACION\033[0m\n");
    printf(
        "  "
        "\033[1;35m═══════════════════════════════════════════════\033[0m\n\n");

    printf("  Particulas nacidas        : %llu\n",
           (unsigned long long)born_total);
    printf("  Particulas promovidas     : %llu  (sobrevivieron >=1 minor GC)\n",
           (unsigned long long)st.promoted_count);
    printf("  Particulas no promovidas  : %llu  (murieron en Nursery)\n",
           (unsigned long long)(born_total - st.promoted_count));
    printf("  Ciclos minor GC           : %llu\n",
           (unsigned long long)st.minor_gc_count);
    printf("  Ciclos major GC           : %llu\n",
           (unsigned long long)st.major_gc_count);
    printf("\n");

    // Visualizacion: proporcion joven vs viejo
    uint64_t died_in_nursery = born_total - st.promoted_count;
    bar("Muertes jovenes", died_in_nursery, born_total);
    bar("Promovidas", st.promoted_count, born_total);
    bar("Liberadas major", st.freed_count,
        st.promoted_count > 0 ? st.promoted_count : 1);

    printf("\n");
    print_stats(st, heap.nursery_used(), heap.nursery_total(), heap.old_used());

    // ---------------------------------------------------------------------------
    // Validaciones
    // ---------------------------------------------------------------------------
    printf("\n  \033[1;36m── Validaciones ──\033[0m\n");

    // La mayoria de particulas debe morir joven (hipotesis generacional)
    double pct_young =
        born_total > 0 ? 100.0 * (double)died_in_nursery / (double)born_total
                       : 0.0;
    printf("  Porcentaje que murio joven: %.1f%%  (esperado >50%%)\n",
           pct_young);
    ASSERT_MSG(pct_young > 50.0,
               "hipotesis generacional: >50% de objetos mueren jovenes");

    // Deben haberse disparado ciclos minor y major GC
    ASSERT_MSG(st.minor_gc_count > 0,
               "al menos 1 ciclo minor GC durante la simulacion");
    ASSERT_MSG(st.major_gc_count > 0,
               "al menos 1 ciclo major GC durante la simulacion");

    // El total de bytes asignados debe cuadrar con born_total
    ASSERT_MSG(st.alloc_bytes == born_total * sizeof(Particle),
               "alloc_bytes == particulas_nacidas * sizeof(Particle)");

    // Tras el major GC final, OldGen no debe crecer indefinidamente
    ASSERT_MSG(heap.old_used() < OLD_THRESHOLD * 2,
               "OldGen bajo control tras major GC final");

    // Los handles soltados deben reflejar las particulas promovidas
    ASSERT_MSG(st.promoted_count <= born_total,
               "promoted_count <= born_total (coherencia)");
}

// ---------------------------------------------------------------------------
// Test de coherencia del grafo de objetos con referencias cruzadas
// ---------------------------------------------------------------------------

/**
 * Construye una lista enlazada circular de N nodos usando handles GC,
 * atraviesa el anillo, luego suelta la mitad de los nodos y verifica
 * que el GC no rompe los nodos supervivientes.
 *
 * Layout del nodo:
 *   [0..3]  value    (uint32_t)
 *   [4..7]  next_h   (GcHandle — uint32_t)
 *   [8..11] id       (uint32_t)
 *   [12..15] _pad
 */

struct Node {
    uint32_t value;
    gc::GcHandle next;
    uint32_t id;
    uint32_t _pad;
};
static_assert(sizeof(Node) == 16, "Node debe medir 16 bytes");

static Node *nget(gc::GcHeap &heap, gc::GcHandle h) {
    return reinterpret_cast<Node *>(heap.deref(h));
}

static void run_linked_list_test() {
    printf(
        "\n\033[1;36m══════════════════════════════════════════════\033[0m\n");
    printf(
        "\033[1;36m  Lista enlazada con referencias entre objetos  \033[0m\n");
    printf(
        "\033[1;36m══════════════════════════════════════════════\033[0m\n\n");

    constexpr int N = 20;
    constexpr size_t NURSERY_SIZE = 16 * 1024;
    constexpr size_t OLD_THRESHOLD = 32 * 1024;

    vm::ArenaManager mgr;
    gc::GcHeap heap(mgr, NURSERY_SIZE, OLD_THRESHOLD);

    // Crear N nodos
    std::vector<gc::GcHandle> nodes(N);
    for (int i = 0; i < N; ++i) {
        nodes[i] = heap.alloc(sizeof(Node));
        Node *n = nget(heap, nodes[i]);
        n->value = (uint32_t)i * 10;
        n->id = (uint32_t)i;
        n->next = gc::GC_NULL_HANDLE; // se enlazara despues
        printf("  alloc nodo[%2d] → handle=%u  value=%u\n", i, nodes[i],
               n->value);
    }

    // Enlazar: cada nodo apunta al siguiente, el ultimo apunta al primero
    for (int i = 0; i < N; ++i) {
        Node *n = nget(heap, nodes[i]);
        n->next = nodes[(i + 1) % N];
    }
    printf("\n  Lista circular de %d nodos creada.\n", N);

    // Atravesar el anillo completo desde el nodo 0 y verificar valores
    printf("  Recorrido completo del anillo:\n  ");
    gc::GcHandle cur = nodes[0];
    int visited = 0;
    do {
        Node *n = nget(heap, cur);
        printf("%u→", n->value);
        cur = n->next;
        visited++;
    } while (cur != nodes[0] && visited <= N + 1);
    printf("(vuelta)\n");
    ASSERT_MSG(visited == N, "recorrido completo: N nodos visitados");

    // Forzar minor GC para promocionar todos a OldGen
    printf("\n  >>> minor_gc() — promocionando todos a OldGen...\n");
    heap.minor_gc();
    ASSERT_MSG(heap.nursery_used() == 0, "Nursery vacia tras minor GC");

    // Los handles siguen validos tras la evacuacion
    printf("  Verificando handles post-evacuacion...\n");
    bool all_valid = true;
    for (int i = 0; i < N; ++i) {
        Node *n = nget(heap, nodes[i]);
        if (!n || n->id != (uint32_t)i || n->value != (uint32_t)i * 10) {
            printf("  FALLO: nodo[%d] corrompido tras minor GC\n", i);
            all_valid = false;
        }
    }
    ASSERT_MSG(all_valid, "todos los nodos intactos tras minor GC");

    // Los next handles siguen resolviendo al nodo correcto tras evacuacion
    printf("  Verificando enlaces next tras evacuacion...\n");
    bool links_ok = true;
    for (int i = 0; i < N; ++i) {
        Node *n = nget(heap, nodes[i]);
        Node *next = nget(heap, n->next);
        if (!next || next->id != (uint32_t)((i + 1) % N)) {
            printf("  FALLO: enlace nodo[%d]→nodo[%d] roto\n", i, (i + 1) % N);
            links_ok = false;
        }
    }
    ASSERT_MSG(links_ok, "todos los enlaces next validos tras minor GC");

    // Soltar los nodos de indice impar (basura)
    printf("\n  Soltando nodos impares (indices 1,3,5,...) como basura...\n");
    int dropped = 0;
    for (int i = 1; i < N; i += 2) {
        heap.drop(nodes[i]);
        nodes[i] = gc::GC_NULL_HANDLE;
        dropped++;
    }
    printf("  %d nodos soltados.\n", dropped);

    // major GC debe recoger los nodos impares
    printf("  >>> major_gc()...\n");
    heap.major_gc();

    const gc::GcStats &st = heap.stats();
    printf("  freed_count tras major GC: %llu  (esperado %d)\n",
           (unsigned long long)st.freed_count, dropped);
    ASSERT_MSG(st.freed_count == (uint64_t)dropped,
               "major GC libero exactamente los nodos soltados");

    // Los nodos pares siguen siendo accesibles y sus datos intactos
    printf("  Verificando nodos pares supervivientes...\n");
    bool survivors_ok = true;
    for (int i = 0; i < N; i += 2) {
        Node *n = nget(heap, nodes[i]);
        if (!n || n->id != (uint32_t)i || n->value != (uint32_t)i * 10) {
            printf("  FALLO: nodo par[%d] corrompido tras major GC\n", i);
            survivors_ok = false;
        }
    }
    ASSERT_MSG(survivors_ok,
               "nodos pares supervivientes intactos tras major GC");

    // Soltar los nodos pares y hacer major GC final: OldGen debe quedar a 0
    printf("\n  Soltando todos los nodos restantes...\n");
    for (int i = 0; i < N; i += 2) {
        if (nodes[i] != gc::GC_NULL_HANDLE) heap.drop(nodes[i]);
    }
    heap.major_gc();
    printf("  old_used tras limpieza total: %zu bytes\n", heap.old_used());
    ASSERT_MSG(heap.old_used() == 0,
               "OldGen vacia tras soltar todos los nodos");

    printf("\n");
    print_stats(st, heap.nursery_used(), heap.nursery_total(), heap.old_used());
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
        "\033[1;35m║   TEST 3 — GcHeap en uso real                ║\033[0m\n");
    printf(
        "\033[1;35m╚══════════════════════════════════════════════╝\033[0m\n");

    run_particle_simulation();
    run_linked_list_test();

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
