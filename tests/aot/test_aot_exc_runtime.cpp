/*
 * VestaVM - test del runtime de excepciones nativo (__vx_setjmp/longjmp).
 *
 * Carga los bytes hand-rolled en memoria RWX y valida el round-trip
 * setjmp/longjmp en el ABI del HOST (Win64 en Windows, SysV en POSIX).
 */

#include "aot/aot_exc_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fails;                                                         \
            std::printf("  FAIL: %s (linea %d)\n", msg, __LINE__);             \
        }                                                                      \
    } while (0)

// Reserva memoria ejecutable y copia los bytes; devuelve el puntero.
static void *make_exec(const std::vector<uint8_t> &code) {
#if defined(_WIN32)
    void *p = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap(nullptr, code.size(), PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = nullptr;
#endif
    if (p) std::memcpy(p, code.data(), code.size());
    return p;
}

typedef int64_t (*setjmp_fn)(void *buf);
typedef void (*longjmp_fn)(void *buf, int64_t val);

// Estado del round-trip via globales (evita que el compilador asuma que
// setjmp retorna una sola vez).
static volatile int g_phase = 0;
static longjmp_fn g_lj = nullptr;
static void *g_buf = nullptr;
static int64_t g_longjmp_val = 0;

// Hace setjmp; si retorna 0 (primera vez) dispara longjmp; el longjmp
// reentra en el setjmp devolviendo g_longjmp_val.  Devuelve el 2o retorno.
static int64_t roundtrip(setjmp_fn sj) {
    volatile int64_t r = sj(g_buf); // 1a vez: 0; tras longjmp: g_longjmp_val
    if (r == 0) {
        g_phase = 1;
        g_lj(g_buf, g_longjmp_val);
        return -999; // inalcanzable
    }
    return r;
}

int main() {
    std::printf("== test_aot_exc_runtime ==\n");

#if defined(_WIN32)
    const bool host_sysv = false;
#else
    const bool host_sysv = true;
#endif

    auto sj_code = aot::aot_exc_setjmp_bytes(host_sysv);
    auto lj_code = aot::aot_exc_longjmp_bytes(host_sysv);
    CHECK(!sj_code.empty(), "setjmp bytes no vacios");
    CHECK(!lj_code.empty(), "longjmp bytes no vacios");

    setjmp_fn sj = reinterpret_cast<setjmp_fn>(make_exec(sj_code));
    longjmp_fn lj = reinterpret_cast<longjmp_fn>(make_exec(lj_code));
    CHECK(sj != nullptr && lj != nullptr, "memoria ejecutable OK");
    if (!sj || !lj) {
        std::printf("== %d checks, %d fallos ==\n", g_checks, g_fails);
        return g_fails ? 1 : 0;
    }

    aot::ExcBufLayout L = aot::aot_exc_buf_layout(host_sysv);
    CHECK(L.total_size > 0 && L.total_size <= 128, "layout razonable");

    // Buffer alineado y holgado.
    alignas(16) uint64_t buf[32];
    g_buf = buf;
    g_lj = lj;

    // Caso 1: longjmp con valor 42 -> setjmp re-retorna 42.
    g_phase = 0;
    g_longjmp_val = 42;
    int64_t r1 = roundtrip(sj);
    CHECK(g_phase == 1, "el longjmp se disparo (phase=1)");
    CHECK(r1 == 42, "setjmp re-retorna el valor del longjmp (42)");

    // Caso 2: longjmp con valor 0 -> setjmp re-retorna 1 (semantica estandar).
    g_phase = 0;
    g_longjmp_val = 0;
    int64_t r2 = roundtrip(sj);
    CHECK(g_phase == 1, "caso 0: longjmp disparado");
    CHECK(r2 == 1, "longjmp(buf,0) -> setjmp re-retorna 1");

    // Caso 3: valor grande (verifica que pasa el qword entero).
    g_phase = 0;
    g_longjmp_val = 0x1234567890ABLL;
    int64_t r3 = roundtrip(sj);
    CHECK(r3 == 0x1234567890ABLL, "valor grande preservado");

    std::printf("== %d checks, %d fallos ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
