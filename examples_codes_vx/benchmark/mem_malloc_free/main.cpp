// mem_malloc_free: 5M bloques de 96 bytes, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; el porque de la ventana esta en
// `alloc_small/main.c`.
#include <cstdlib>
#include <cstdint>

static const int TAM = 96;
static const int32_t ITERS = 5000000;
static const int VIVOS = 64;   // potencia de 2

int main() {
    uint8_t *anillo[VIVOS];
    for (int k = 0; k < VIVOS; k++) anillo[k] = nullptr;

    int acc = 0;
    for (int32_t i = 0; i < ITERS; ++i) {
        uint8_t *buf = (uint8_t *) std::malloc(TAM);
        buf[0] = (uint8_t) i;
        buf[TAM - 1] = (uint8_t) (i + TAM - 1);
        int k = i & (VIVOS - 1);
        if (anillo[k] != nullptr) {    // el mas viejo sale de la ventana
            acc += anillo[k][0] + anillo[k][TAM - 1];
            std::free(anillo[k]);
        }
        anillo[k] = buf;
    }
    for (int k = 0; k < VIVOS; k++) {  // vaciar la ventana
        if (anillo[k] != nullptr) {
            acc += anillo[k][0] + anillo[k][TAM - 1];
            std::free(anillo[k]);
        }
    }
    return acc % 251;
}
