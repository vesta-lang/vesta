// cmp_fusion: 50M comparaciones + salto, con el resultado consumido.
// Mismo algoritmo que main.c; ver alli por que la condicion tiene que ser
// impredecible en compilacion (antes `g++ -O3` resolvia el bucle en forma
// cerrada y el bench no ejecutaba nada).
#include <cstdint>

static const int32_t ITERS = 50000000;

int main() {
    uint32_t s = 12345u;
    int32_t acc = 0;
    for (int32_t i = 0; i < ITERS; i++) {
        s = s * 1664525u + 1013904223u;
        if ((s >> 31) == 0u) acc++;
    }
    return acc & 0xFF;
}
