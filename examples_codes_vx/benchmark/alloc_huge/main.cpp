// alloc_huge: 100 bloques de 16 MB, con una ventana de 4 vivos.
// Mismo algoritmo que main.c; ver alli por que a este tamano lo que se mide es
// el fallo de pagina y no la llamada al asignador.
#include <cstdlib>
#include <cstddef>

static const size_t TAM = 16u * 1024u * 1024u;
static const int ITERS = 100;
static const int VIVOS = 4;       // potencia de 2
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
