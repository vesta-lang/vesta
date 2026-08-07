// hash_lookup: 50M mezclas estilo FNV, con el resultado consumido.
// Mismo algoritmo que main.c; ver alli los DOS problemas que tenia: la
// condicion que no se cumplia nunca (`seed |= 1` y luego `seed & 7 == 0`) y el
// borrado de la mezcla que aquella provocaba.
#include <cstdint>

static const int32_t ITERS = 50000000;

int main() {
    uint64_t seed = 0xCAFEBABEDEADBEEFULL;
    uint64_t acc = 0;
    for (int32_t i = 0; i < ITERS; ++i) {
        seed ^= (uint64_t) i;
        seed *= 1099511628211ULL;
        seed >>= 7;
        seed |= 1ULL;
        acc += seed & 0xFFULL;
    }
    return (int32_t)(acc % 251);
}
