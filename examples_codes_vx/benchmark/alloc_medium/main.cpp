// alloc_medium: 4M bloques de 1 KB, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; el porque del diseno esta en
// `alloc_small/main.c`.
#include <cstdlib>
#include <cstddef>

static const size_t TAM = 1024;
static const int ITERS = 4000000;
static const int VIVOS = 64;      // potencia de 2
static const size_t PAGINA = 4096;

int main() {
    unsigned char *anillo[VIVOS];
    for (int k = 0; k < VIVOS; k++) anillo[k] = nullptr;

    int acc = 0;
    for (int i = 0; i < ITERS; i++) {
        unsigned char *p = (unsigned char*)std::malloc(TAM);
        unsigned char v = (unsigned char)(i & 0xFF);
        for (size_t o = 0; o < TAM; o += PAGINA) p[o] = v;
        p[TAM - 1] = v;
        int k = i & (VIVOS - 1);
        if (anillo[k] != nullptr) {    // el mas viejo sale de la ventana
            acc += anillo[k][0];
            std::free(anillo[k]);
        }
        anillo[k] = p;
    }
    for (int k = 0; k < VIVOS; k++) {  // vaciar la ventana
        if (anillo[k] != nullptr) { acc += anillo[k][0]; std::free(anillo[k]); }
    }
    return acc & 0xFF;
}
