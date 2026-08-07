// mem_struct: 1M iteraciones x 3 caminos (pila, heap por sizeof, heap por
// tamano explicito), con ventana de 64 vivos en los dos de heap.
// Mismo algoritmo que main.c; ver alli que compara y por que la ventana.
#include <cstdlib>

static const int ITERS = 1000000;
static const int VIVOS = 64;   // potencia de 2

struct Punto { int x; int y; };

int main() {
    Punto *anillo_a[VIVOS];
    Punto *anillo_b[VIVOS];
    for (int k = 0; k < VIVOS; k++) { anillo_a[k] = nullptr; anillo_b[k] = nullptr; }

    long long acc = 0;
    for (int i = 0; i < ITERS; i++) {
        int base = (int)(acc & 0xFF);

        Punto p;                       // 1. en la pila
        p.x = base;
        p.y = base + 1;
        acc += p.x + p.y;

        int k = i & (VIVOS - 1);

        Punto *h = (Punto*)std::malloc(sizeof(Punto));   // 2. heap por sizeof
        h->x = base; h->y = base + 1;
        if (anillo_a[k] != nullptr) {
            acc += anillo_a[k]->x + anillo_a[k]->y;
            std::free(anillo_a[k]);
        }
        anillo_a[k] = h;

        Punto *m = (Punto*)std::malloc(8);               // 3. heap explicito
        m->x = base; m->y = base + 1;
        if (anillo_b[k] != nullptr) {
            acc += anillo_b[k]->x + anillo_b[k]->y;
            std::free(anillo_b[k]);
        }
        anillo_b[k] = m;
    }
    for (int k = 0; k < VIVOS; k++) {
        if (anillo_a[k] != nullptr) {
            acc += anillo_a[k]->x + anillo_a[k]->y; std::free(anillo_a[k]);
        }
        if (anillo_b[k] != nullptr) {
            acc += anillo_b[k]->x + anillo_b[k]->y; std::free(anillo_b[k]);
        }
    }
    return (int)(acc % 251);
}
